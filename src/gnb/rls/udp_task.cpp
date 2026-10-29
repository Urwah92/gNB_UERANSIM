//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include "udp_task.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <unistd.h>

#include <gnb/nts.hpp>
#include <utils/common.hpp>
#include <utils/constants.hpp>
#include <utils/libc_error.hpp>
#include <utils/process.hpp>
#include <algorithm>
#include <limits>
#include <string>

#include <arpa/inet.h>
#include <cstring>
#include <stdexcept> 

#include <cstdlib>

static constexpr const int BUFFER_SIZE = 16384;

static constexpr const int LOOP_PERIOD = 1000;
static constexpr const int RECEIVE_TIMEOUT = 200;
static constexpr const int HEARTBEAT_THRESHOLD = 5000; // (LOOP_PERIOD + RECEIVE_TIMEOUT)'dan büyük olmalı

static constexpr const int MIN_ALLOWED_DBM = -120;
static std::string PeerIpv4(const InetAddress &address)
{
    if (address.getIpVersion() != 4)
        return {};
    char buffer[INET_ADDRSTRLEN]{};
    const auto *peer = reinterpret_cast<const sockaddr_in *>(address.getSockAddr());
    return inet_ntop(AF_INET, &peer->sin_addr, buffer, sizeof(buffer)) ? buffer : "";
}

static int EstimateSimulatedDbm(const Vector3 &myPos, const Vector3 &uePos)
{
    const double x = double(myPos.x) - uePos.x;
    const double y = double(myPos.y) - uePos.y;
    const double z = double(myPos.z) - uePos.z;
    const double distance = std::sqrt(x * x + y * y + z * z);
    return -std::max(1, static_cast<int>(std::min(distance, double(INT32_MAX))));
}

namespace nr::gnb
{

RlsUdpTask::RlsUdpTask(TaskBase *base, uint64_t sti, Vector3 phyLocation, bool wifi, std::string ueInterface, std::string interface)
    : m_server{}, m_ctlTask{}, m_sti{sti}, m_phyLocation{phyLocation}, m_lastLoop{}, m_stiToUe{}, m_ueMap{}, m_newIdCounter{}
    , m_wifi{wifi}, m_ueInterface{ueInterface},m_interface{interface}
{
    m_logger = base->logBase->makeUniqueLogger("rls-udp");
    m_chain = "UERANSIM_" + std::to_string(base->config->getGnbId());

    m_server = new udp::UdpServer(base->config->linkIp, cons::RadioLinkPort);
}

void RlsUdpTask::onStart()
{
    if (!m_wifi)
        return;
    if (utils::RunCommand({"iptables", "-w", "-N", m_chain}) != 0)
    {
        m_logger->err("Cannot create AP firewall chain[%s]; use a distinct gNB ID and root privileges", m_chain.c_str());
        return;
    }
    m_ownsFirewall = true;
    if (utils::RunCommand({"iptables", "-w", "-A", m_chain, "-j", "DROP"}) != 0 ||
        utils::RunCommand({"iptables", "-w", "-I", "FORWARD", "1", "-i", m_ueInterface,
                           "-o", m_interface, "-j", m_chain}) != 0 ||
        utils::RunCommand({"iptables", "-w", "-I", "FORWARD", "1", "-i", m_interface,
                           "-o", m_ueInterface, "-j", m_chain}) != 0)
    {
        m_logger->err("Failed to configure AP forwarding; cleaning up owned rules");
        clearWifiForwarding();
        return;
    }
    m_firewallReady = true;
}

void RlsUdpTask::onLoop()
{
    auto current = utils::CurrentTimeMillis();
    if (current - m_lastLoop > LOOP_PERIOD)
    {
        m_lastLoop = current;
        heartbeatCycle(current);
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
    clearWifiForwarding();
    delete m_server;
}

void RlsUdpTask::receiveRlsPdu(const InetAddress &addr, std::unique_ptr<rls::RlsMessage> &&msg)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (msg->msgType == rls::EMessageType::HEARTBEAT)
    {
        const int dbm = EstimateSimulatedDbm(m_phyLocation, ((const rls::RlsHeartBeat &)*msg).simPos);
        if (m_wifi)
        {
            const auto address = PeerIpv4(addr);
            if (!address.empty())
            {
                setWifiForwarding(address, dbm > MIN_ALLOWED_DBM);
                if (dbm > MIN_ALLOWED_DBM)
                    m_wifiLastSeen[address] = utils::CurrentTimeMillis();
            }
            rls::RlsHeartBeatAck ack{m_sti};
            ack.dbm = dbm > MIN_ALLOWED_DBM && !m_forwardedAddresses.count(address) ? MIN_ALLOWED_DBM : dbm;
            sendRlsPdu(addr, ack);
            return;
        }
        if (dbm < MIN_ALLOWED_DBM)
        {
            auto peer = m_stiToUe.find(msg->sti);
            if (peer != m_stiToUe.end())
            {
                const int ueId = peer->second;
                const auto now = utils::CurrentTimeMillis();
                m_ueMap[ueId].address = addr;
                m_ueMap[ueId].lastSeen = now;
                if (!m_releaseTimes.count(ueId) || now - m_releaseTimes[ueId] >= 10000)
                {
                    auto request = std::make_unique<NmGnbRlsToRls>(NmGnbRlsToRls::SESSION_CHANGE);
                    request->ueId = ueId;
                    request->psi = 0; // Export all active PDU sessions without moving them out of GTP.
                    m_releaseTimes[ueId] = now;
                    m_ctlTask->push(std::move(request));
                }
            }
            return;
        }
        auto peer = m_stiToUe.find(msg->sti);
        if (peer != m_stiToUe.end())
            m_releaseTimes.erase(peer->second);
        if (m_stiToUe.count(msg->sti))
        {
            int ueId = m_stiToUe[msg->sti];
            m_ueMap[ueId].sti = msg->sti;
            m_ueMap[ueId].address = addr;
            m_ueMap[ueId].lastSeen = utils::CurrentTimeMillis();
        }
        else
        {
            int ueId = ++m_newIdCounter;

            m_stiToUe[msg->sti] = ueId;
            m_ueMap[ueId].sti = msg->sti;
            m_ueMap[ueId].address = addr;
            m_ueMap[ueId].lastSeen = utils::CurrentTimeMillis();

            auto w = std::make_unique<NmGnbRlsToRls>(NmGnbRlsToRls::SIGNAL_DETECTED);
            w->ueId = ueId;
            m_ctlTask->push(std::move(w));
        }

        rls::RlsHeartBeatAck ack{m_sti};
        ack.dbm = dbm;

        sendRlsPdu(addr, ack);
        return;
    }

    if (m_wifi || !m_stiToUe.count(msg->sti))
    {
        // if no HB received yet, and the message is not HB, then ignore the message
        return;
    }

    auto w = std::make_unique<NmGnbRlsToRls>(NmGnbRlsToRls::RECEIVE_RLS_MESSAGE);
    w->ueId = m_stiToUe[msg->sti];
    w->msg = std::move(msg);
    m_ctlTask->push(std::move(w));
}

void RlsUdpTask::sendRlsPdu(const InetAddress &addr, const rls::RlsMessage &msg)
{
    OctetString stream;
    rls::EncodeRlsMessage(msg, stream);

    m_server->Send(addr, stream.data(), static_cast<size_t>(stream.length()));
}

void RlsUdpTask::heartbeatCycle(int64_t time)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_wifi)
    {
        std::vector<std::string> expired;
        for (const auto &entry : m_wifiLastSeen)
            if (time - entry.second > HEARTBEAT_THRESHOLD)
                expired.push_back(entry.first);
        for (const auto &address : expired)
            setWifiForwarding(address, false);
        return;
    }
    std::set<int> lostUeId{};
    std::set<uint64_t> lostSti{};

    for (auto &item : m_ueMap)
    {
        if (time - item.second.lastSeen > HEARTBEAT_THRESHOLD)
        {
            lostUeId.insert(item.first);
            lostSti.insert(item.second.sti);
        }
    }

    for (uint64_t sti : lostSti)
        m_stiToUe.erase(sti);

    for (int ueId : lostUeId)
    {
        m_ueMap.erase(ueId);
        m_releaseTimes.erase(ueId);
    }

    for (int ueId : lostUeId)
    {
        auto w = std::make_unique<NmGnbRlsToRls>(NmGnbRlsToRls::SIGNAL_LOST);
        w->ueId = ueId;
        m_ctlTask->push(std::move(w));
    }
}

void RlsUdpTask::initialize(NtsTask *ctlTask)
{
    m_ctlTask = ctlTask;
}

void RlsUdpTask::send(int ueId, const rls::RlsMessage &msg)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (ueId == 0)
    {
        for (auto &ue : m_ueMap)
            send(ue.first, msg);
        return;
    }

    if (!m_ueMap.count(ueId))
    {
        // ignore the message
        return;
    }

    sendRlsPdu(m_ueMap[ueId].address, msg);
}

void RlsUdpTask::setWifiForwarding(const std::string &address, bool enabled)
{
    if (!m_firewallReady || m_forwardedAddresses.count(address) == static_cast<size_t>(enabled))
        return;
    auto rule = [&](const std::string &operation, bool uplink) {
        return utils::RunCommand({"iptables", "-w", operation, m_chain, "-i", uplink ? m_ueInterface : m_interface,
            "-o", uplink ? m_interface : m_ueInterface, uplink ? "-s" : "-d", address + "/32", "-j", "ACCEPT"});
    };
    if (enabled)
    {
        if (rule("-I", true) != 0)
            return;
        if (rule("-I", false) != 0)
        {
            rule("-D", true);
            return;
        }
        m_forwardedAddresses.insert(address);
        m_logger->info("Wi-Fi forwarding enabled for UE[%s]", address.c_str());
    }
    else
    {
        const bool uplinkRemoved = rule("-D", true) == 0 || rule("-C", true) == 1;
        const bool downlinkRemoved = rule("-D", false) == 0 || rule("-C", false) == 1;
        if (!uplinkRemoved || !downlinkRemoved)
        {
            m_logger->err("Failed to remove Wi-Fi rules for UE[%s]; will retry", address.c_str());
            return;
        }
        m_forwardedAddresses.erase(address);
        m_wifiLastSeen.erase(address);
        m_logger->info("Wi-Fi forwarding removed for UE[%s]", address.c_str());
    }
}

void RlsUdpTask::clearWifiForwarding()
{
    if (!m_ownsFirewall)
        return;
    utils::RunCommand({"iptables", "-w", "-D", "FORWARD", "-i", m_ueInterface, "-o", m_interface, "-j", m_chain});
    utils::RunCommand({"iptables", "-w", "-D", "FORWARD", "-i", m_interface, "-o", m_ueInterface, "-j", m_chain});
    utils::RunCommand({"iptables", "-w", "-F", m_chain});
    utils::RunCommand({"iptables", "-w", "-X", m_chain});
    m_firewallReady = false;
    m_ownsFirewall = false;
    m_forwardedAddresses.clear();
    m_wifiLastSeen.clear();
}

} // namespace nr::gnb
