//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//
#include <iostream>
#include <ue/types.hpp>
#include "task.hpp"
#include "cmd_handler.hpp"
#include <lib/nas/utils.hpp>
#include <ue/nas/task.hpp>
#include <ue/rls/task.hpp>
#include <ue/tun/tun.hpp>
#include <utils/common.hpp>
#include <utils/constants.hpp>
#include <utils/process.hpp>
#include <unistd.h>

#include <sstream>
#include <vector>
#include <string>
#include <stdexcept>

#include <ifaddrs.h>    // For getifaddrs, freeifaddrs
#include <arpa/inet.h>  // For inet_ntoa (used for converting IP address to string)


static constexpr const int SWITCH_OFF_TIMER_ID = 1;
static constexpr const int SWITCH_OFF_DELAY = 500;

namespace nr::ue
{

UeAppTask::UeAppTask(TaskBase *base) : m_base{base}
{
    m_logger = m_base->logBase->makeUniqueLogger(m_base->config->getLoggerPrefix() + "app");
}

void UeAppTask::onStart()
{
    // Wi-Fi uses the configured Linux interface; 5G creates TUNs after session establishment.
}

void UeAppTask::onQuit()
{
    removeWifiRoute();
    for (auto &tunTask : m_tunTasks)
    {
        if (tunTask != nullptr)
        {
            tunTask->quit();
            delete tunTask;
            tunTask = nullptr;
        }
    }
}

void UeAppTask::onLoop()
{
    auto msg = take();
    if (!msg)
    {
        if (m_base->config->wifi)
            refreshWifiRoute();
        return;
    }
    switch (msg->msgType)
    {
    case NtsMessageType::UE_TUN_TO_APP: {
        auto &w = dynamic_cast<NmUeTunToApp &>(*msg);
        switch (w.present)
        {
        case NmUeTunToApp::DATA_PDU_DELIVERY: {
            auto m = std::make_unique<NmUeAppToNas>(NmUeAppToNas::UPLINK_DATA_DELIVERY);
            m->psi = w.psi;
            m->data = std::move(w.data);
            m_base->nasTask->push(std::move(m));
            break;
        }
        case NmUeTunToApp::TUN_ERROR: {
            m_logger->err("TUN failure [%s]", w.error.c_str());
            break;
        }
        }
        break;
    }
    case NtsMessageType::UE_NAS_TO_APP: {
        auto &w = dynamic_cast<NmUeNasToApp &>(*msg);
        switch (w.present)
        {
        case NmUeNasToApp::PERFORM_SWITCH_OFF: {
            setTimer(SWITCH_OFF_TIMER_ID, SWITCH_OFF_DELAY);
            break;
        }
        case NmUeNasToApp::RADIO_LINK_FAILURE:
            // Preserve session TUNs until NAS explicitly releases them or mobility recovers.
            break;
        case NmUeNasToApp::DOWNLINK_DATA_DELIVERY: {
            auto *tunTask = m_tunTasks[w.psi];
            if (tunTask)
            {
                auto m = std::make_unique<NmAppToTun>(NmAppToTun::DATA_PDU_DELIVERY);
                m->psi = w.psi;
                m->data = std::move(w.data);
                tunTask->push(std::move(m));
            }
            break;
        }
        }
        break;
    }
    //Urwah Edition
    case NtsMessageType::UE_RRC_TO_APP: {
        auto &w = dynamic_cast<NmUeRrcToApp &>(*msg);
        switch (w.present)
        {
        case NmUeRrcToApp::SWITCH_REQUEST: {
            updateRoutingForTunInterface(w.psi);
            auto x = std::make_unique<NmUeAppToNas>(NmUeAppToNas::SESSION_SWITCH_REQUEST);
            x->psi = w.psi;
            m_base->nasTask->push(std::move(x)); 
            break;
        }
        }
        break;
    }
    case NtsMessageType::UE_STATUS_UPDATE: {
        receiveStatusUpdate(dynamic_cast<NmUeStatusUpdate &>(*msg));
        break;
    }
    case NtsMessageType::UE_CLI_COMMAND: {
        auto &w = dynamic_cast<NmUeCliCommand &>(*msg);
        UeCmdHandler handler{m_base};
        handler.handleCmd(w);
        break;
    }
    case NtsMessageType::TIMER_EXPIRED: {
        auto &w = dynamic_cast<NmTimerExpired &>(*msg);
        if (w.timerId == SWITCH_OFF_TIMER_ID)
        {
            m_logger->info("UE device is switching off");
            m_base->ueController->performSwitchOff(m_base->ue);
        }
        break;
    }
    default:
        m_logger->unhandledNts(*msg);
        break;
    }
}

void UeAppTask::receiveStatusUpdate(NmUeStatusUpdate &msg)
{
    if (msg.what == NmUeStatusUpdate::SESSION_ESTABLISHMENT)
    {
        auto *session = msg.pduSession;
        setupTunInterface(session);
        return;
    }

    if (msg.what == NmUeStatusUpdate::SESSION_RELEASE)
    {
        if (m_tunTasks[msg.psi] != nullptr)
        {
            m_tunTasks[msg.psi]->quit();
            delete m_tunTasks[msg.psi];
            m_tunTasks[msg.psi] = nullptr;
        }

        return;
    }

    if (msg.what == NmUeStatusUpdate::CM_STATE)
    {
        m_cmState = msg.cmState;
        return;
    }
}

void UeAppTask::setupTunInterface(const PduSession *pduSession)
{
    if (!utils::IsRoot())
    {
        m_logger->err("TUN interface could not be setup. Permission denied. Please run the UE with 'sudo'");
        return;
    }

    if (!pduSession->pduAddress.has_value())
    {
        m_logger->err("Connection could not setup. PDU address is missing.");
        return;
    }

    if (pduSession->pduAddress->sessionType != nas::EPduSessionType::IPV4 ||
        pduSession->sessionType != nas::EPduSessionType::IPV4)
    {
        m_logger->err("Connection could not setup. PDU session type is not supported.");
        return;
    }

    int psi = pduSession->psi;
    if (psi == 0 || psi > 15)
    {
        m_logger->err("Connection could not setup. Invalid PSI.");
        return;
    }

    if (m_tunTasks[psi] != nullptr)
    {
        m_logger->err("Connection could not setup. TUN task for specified PSI is non-null.");
        return;
    }

    std::string error{}, allocatedName{};
    std::string requestedName = cons::TunNamePrefix;
    if (m_base->config->tunName.has_value())
        requestedName = *m_base->config->tunName;
    int fd = tun::TunAllocate(requestedName.c_str(), allocatedName, error);
    if (fd == 0 || error.length() > 0)
    {
        m_logger->err("TUN allocation failure [%s]", error.c_str());
        return;
    }
 
    std::string ipAddress = utils::OctetStringToIp(pduSession->pduAddress->pduAddressInformation);

    bool r = tun::TunConfigure(allocatedName, ipAddress, cons::TunMtu, m_base->config->configureRouting, error);
    if (!r || error.length() > 0)
    {
        m_logger->err("TUN configuration failure [%s]", error.c_str());
        close(fd);
        return;
    }

    auto *task = new TunTask(m_base, psi, fd);
    m_tunTasks[psi] = task;
    task->start();
    m_tunNames[psi] = allocatedName;
    m_tunAddresses[psi] = ipAddress;

    m_logger->info("Connection setup for PDU session[%d] is successful, TUN interface[%s, %s] is up.", pduSession->psi,
                   allocatedName.c_str(), ipAddress.c_str());
}

void UeAppTask::updateRoutingForTunInterface(int psi)
{
    if (psi < 1 || psi > 15 || m_tunTasks[psi] == nullptr)
        return;
    std::string error;
    if (!tun::TunConfigure(m_tunNames[psi], m_tunAddresses[psi], cons::TunMtu,
                           m_base->config->configureRouting, error))
        m_logger->err("Failed to update routing for PDU session[%d]: %s", psi, error.c_str());
}

void UeAppTask::refreshWifiRoute()
{
    std::string gateway;
    int bestDbm = -120;
    const auto now = utils::CurrentTimeMillis();
    {
        std::lock_guard<std::mutex> lock(m_base->wifiState.mutex);
        for (const auto &candidate : m_base->wifiState.candidates)
        {
            if (now - candidate.lastSeen <= 10000 && candidate.dbm > bestDbm)
            {
                gateway = candidate.address;
                bestDbm = candidate.dbm;
            }
        }
    }
    if (gateway == m_wifiGateway)
        return;
    if (gateway.empty())
    {
        removeWifiRoute();
        return;
    }
    const auto &config = *m_base->config;
    const auto table = std::to_string(config.wifiRoutingTable);
    const auto source = config.staticIP + "/32";
    if (utils::RunCommand({"ip", "route", "replace", "table", table, "default", "via", gateway,
                           "dev", config.interface, "onlink", "proto", "186"}) != 0)
    {
        m_logger->err("Failed to set Wi-Fi route in table[%s]", table.c_str());
        return;
    }
    if (!m_wifiRuleInstalled)
    {
        if (utils::RunCommand({"ip", "rule", "add", "priority", table, "from", source, "table", table}) != 0)
        {
            utils::RunCommand({"ip", "route", "del", "table", table, "default", "via", gateway,
                               "dev", config.interface, "proto", "186"});
            m_logger->err("Failed to add Wi-Fi policy rule for source[%s]", source.c_str());
            return;
        }
        m_wifiRuleInstalled = true;
    }
    m_wifiGateway = gateway;
    m_logger->info("Wi-Fi gateway[%s] selected, source[%s], routing table[%s]",
                   gateway.c_str(), config.staticIP.c_str(), table.c_str());
}

void UeAppTask::removeWifiRoute()
{
    if (!m_wifiRuleInstalled)
        return;
    const auto &config = *m_base->config;
    const auto table = std::to_string(config.wifiRoutingTable);
    utils::RunCommand({"ip", "rule", "del", "priority", table, "from", config.staticIP + "/32", "table", table});
    utils::RunCommand({"ip", "route", "del", "table", table, "default", "via", m_wifiGateway,
                       "dev", config.interface, "proto", "186"});
    m_wifiRuleInstalled = false;
    m_wifiGateway.clear();
}

} // namespace nr::ue
