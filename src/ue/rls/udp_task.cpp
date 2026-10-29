//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include "udp_task.hpp"
#include "mobility.hpp"
#include <cstdint>
#include <cstring>
#include <set>
#include <cstdlib>
#include <cmath>
#include <ue/app/task.hpp>
#include <ue/nts.hpp>
#include <utils/common.hpp>
#include <utils/constants.hpp>
#include <numeric>
#include <arpa/inet.h>
#include <cstring>
#include <stdexcept> // For std::runtime_error

static constexpr const int BUFFER_SIZE = 16384;
static constexpr const int LOOP_PERIOD = 1000;
static constexpr const int RECEIVE_TIMEOUT = 200;
static constexpr const int HEARTBEAT_THRESHOLD = 10000; // (LOOP_PERIOD + RECEIVE_TIMEOUT)'dan büyük olmalı

std::string getIPv4AddressString(const InetAddress &inetAddress) {
  if (inetAddress.getIpVersion() != 4) {
    return ""; // Not an IPv4 address
  }

  // Check if getSockAddr returns a null pointer (potential error)
  const sockaddr *addr = inetAddress.getSockAddr();
  if (addr == nullptr) {
    throw std::runtime_error("InetAddress object does not contain a valid address");
  }

  // Cast to const sockaddr_in* only if the IP version is 4
  const sockaddr_in *sin = reinterpret_cast<const sockaddr_in *>(addr);

  char ipString[INET_ADDRSTRLEN]; // Buffer to store string address
  if (inet_ntop(AF_INET, &(sin->sin_addr), ipString, INET_ADDRSTRLEN) == nullptr) {
    // Handle inet_ntop error (unlikely, but possible)
    throw std::runtime_error("Error converting address to string");
  }

  return std::string(ipString);
}

namespace nr::ue
{

RlsUdpTask::RlsUdpTask(TaskBase *base, RlsSharedContext *shCtx, const std::vector<std::string> &searchSpace, std::vector<Vector3> simPos, const std::string mobPattern,const float velocity, const bool wifi)
    : m_base{base}, m_server{}, m_ctlTask{}, m_shCtx{shCtx}, m_searchSpace{}, m_cells{}, m_cellIdToSti{}, m_lastLoop{},m_simPos{simPos},
      m_mobPattern{mobPattern}, m_cellIdCounter{}, m_velocity{velocity}, m_wifi{wifi}
{
    m_logger = base->logBase->makeUniqueLogger(base->config->getLoggerPrefix() + "rls-udp");

    m_server = wifi ? new udp::UdpServer(base->config->staticIP, 0) : new udp::UdpServer();
    for (auto &ip : searchSpace)
        m_searchSpace.emplace_back(ip, cons::RadioLinkPort);
}

void RlsUdpTask::onStart()
{
    // Defensive fallback also covers callers that do not use the YAML parser.
    if (m_simPos.empty())
        m_simPos.emplace_back(0, 0, 0);
    m_movementStart = utils::CurrentTimeMillis();
}

void RlsUdpTask::onLoop()
{
    auto current = utils::CurrentTimeMillis();
    if (current - m_lastLoop >= LOOP_PERIOD)
    {
        m_lastLoop = current;
        const double elapsed = std::max<int64_t>(0, current - m_movementStart) / 1000.0;
        heartbeatCycle(current, SampleMobilityPosition(m_simPos, m_velocity, elapsed, m_mobPattern == "loop"));
    }
    uint8_t buffer[BUFFER_SIZE];
    InetAddress peerAddress;

    int size = m_server->Receive(buffer, BUFFER_SIZE, RECEIVE_TIMEOUT, peerAddress);
    if (size > 0)
    {
        auto rlsMsg = rls::DecodeRlsMessage(OctetView{buffer, static_cast<size_t>(size)});
        if (rlsMsg == nullptr)
            m_logger->err("Unable to decode RLS message");
        else
            receiveRlsPdu(peerAddress, std::move(rlsMsg));
    }
}

void RlsUdpTask::onQuit()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    delete m_server;
    m_server = nullptr;
}

void RlsUdpTask::sendRlsPdu(const InetAddress &addr, const rls::RlsMessage &msg)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_server)
        return;
    OctetString stream;
    rls::EncodeRlsMessage(msg, stream);

    m_server->Send(addr, stream.data(), static_cast<size_t>(stream.length()));
}

void RlsUdpTask::send(int cellId, const rls::RlsMessage &msg)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_cellIdToSti.count(cellId))
    {
        auto sti = m_cellIdToSti[cellId];
        sendRlsPdu(m_cells[sti].address, msg);
    }
}

void RlsUdpTask::receiveRlsPdu(const InetAddress &addr, std::unique_ptr<rls::RlsMessage> &&msg)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (msg->msgType == rls::EMessageType::HEARTBEAT_ACK)
    {
        if (!m_cells.count(msg->sti))
        {
            m_cells[msg->sti].cellId = ++m_cellIdCounter;
            m_cellIdToSti[m_cells[msg->sti].cellId] = msg->sti;
        }

        int oldDbm = INT32_MIN;
        if (m_cells.count(msg->sti))
            oldDbm = m_cells[msg->sti].dbm;

        m_cells[msg->sti].address = addr;
        m_cells[msg->sti].lastSeen = utils::CurrentTimeMillis();

        int newDbm = ((const rls::RlsHeartBeatAck &)*msg).dbm;
        m_cells[msg->sti].dbm = newDbm;
        if (m_wifi)
        {
            const auto address = getIPv4AddressString(addr);
            if (address.empty())
                return;
            std::lock_guard<std::mutex> lock(m_base->wifiState.mutex);
            auto &candidates = m_base->wifiState.candidates;
            auto candidate = std::find_if(candidates.begin(), candidates.end(), [&address](const auto &value) {
                return value.address == address;
            });
            if (candidate == candidates.end())
                candidates.push_back({address, newDbm, utils::CurrentTimeMillis()});
            else
            {
                candidate->dbm = newDbm;
                candidate->lastSeen = utils::CurrentTimeMillis();
            }
            return;
        }
        if (oldDbm != newDbm)
           {onSignalChangeOrLost(m_cells[msg->sti].cellId);}
	    return;
    }

    if (m_wifi)
        return;

    if (!m_cells.count(msg->sti))
    {
        // if no HB-ACK received yet, and the message is not HB-ACK, then ignore the message
        return;
    }

    auto w = std::make_unique<NmUeRlsToRls>(NmUeRlsToRls::RECEIVE_RLS_MESSAGE);
    if (msg->msgType == rls::EMessageType::SESSION_TRANSMISSION)
    {
        w->old_cellId = m_cells[msg->sti].cellId;
        w->cellId = findCellWithHighestDbm(w->old_cellId, utils::CurrentTimeMillis());
        if (w->cellId < 0)
        {
            m_logger->warn("No fresh target cell in coverage for handover");
            return;
        }
    }
    else{
        w->cellId = m_cells[msg->sti].cellId;
    }
    
    w->msg = std::move(msg);
    m_ctlTask->push(std::move(w));
}

void RlsUdpTask::onSignalChangeOrLost(int cellId)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_wifi)
        return;
    int dbm = INT32_MIN;
    if (m_cellIdToSti.count(cellId))
    {
        auto sti = m_cellIdToSti[cellId];
        dbm = m_cells[sti].dbm;
    }

    auto w = std::make_unique<NmUeRlsToRls>(NmUeRlsToRls::SIGNAL_CHANGED);
    w->cellId = cellId;
    w->dbm = dbm;
    m_ctlTask->push(std::move(w));

}

void RlsUdpTask::heartbeatCycle(uint64_t time, const Vector3 &simPos)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_wifi)
    {
        std::lock_guard<std::mutex> lock(m_base->wifiState.mutex);
        auto &candidates = m_base->wifiState.candidates;
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [time](const auto &candidate) {
            return time - candidate.lastSeen > HEARTBEAT_THRESHOLD;
        }), candidates.end());
    }

    std::set<std::pair<uint64_t, int>> toRemove;

    for (auto &cell : m_cells)
    {
        auto delta = time - cell.second.lastSeen;
        if (delta >  HEARTBEAT_THRESHOLD)
            toRemove.insert({cell.first, cell.second.cellId});
    }

    for (auto cell : toRemove)
    {
        m_cells.erase(cell.first);
        m_cellIdToSti.erase(cell.second);
    }

    for (auto cell : toRemove)
       { onSignalChangeOrLost(cell.second);}

    for (auto &addr : m_searchSpace)
    {
        rls::RlsHeartBeat msg{m_shCtx->sti};
        msg.simPos = simPos;
        sendRlsPdu(addr, msg);
    }
}

void RlsUdpTask::initialize(NtsTask *ctlTask)
{
    m_ctlTask = ctlTask;
}

// Function to find the cell with the highest dBm
int RlsUdpTask::findCellWithHighestDbm(int sourceCell, int64_t now)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    int maxDbm = -120;
    int cellWithMaxDbm = -1;
    for (const auto &[key, cellInfo] : m_cells)
    {
        if (cellInfo.cellId == sourceCell || cellInfo.dbm <= -120 || now < cellInfo.lastSeen ||
            now - cellInfo.lastSeen > HEARTBEAT_THRESHOLD)
            continue;
        if (cellInfo.dbm > maxDbm || (cellInfo.dbm == maxDbm && cellInfo.cellId < cellWithMaxDbm))
        {
            maxDbm = cellInfo.dbm;
            cellWithMaxDbm = cellInfo.cellId;
        }
    }

    return cellWithMaxDbm;
}

} // namespace nr::ue
