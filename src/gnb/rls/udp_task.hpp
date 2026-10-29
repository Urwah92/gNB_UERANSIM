//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#pragma once

#include <cstdint>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include <gnb/types.hpp>
#include <lib/rls/rls_pdu.hpp>
#include <lib/udp/server.hpp>
#include <utils/nts.hpp>

namespace nr::gnb
{

class RlsUdpTask : public NtsTask
{
  private:
    struct UeInfo
    {
        uint64_t sti{};
        InetAddress address;
        int64_t lastSeen{};
    };

  private:
    std::unique_ptr<Logger> m_logger;
    udp::UdpServer *m_server;
    NtsTask *m_ctlTask;
    uint64_t m_sti;
    Vector3 m_phyLocation;
    int64_t m_lastLoop;
    std::unordered_map<uint64_t, int> m_stiToUe;
    std::unordered_map<int, UeInfo> m_ueMap;
    int m_newIdCounter;
    bool m_wifi;
    std::string m_ueInterface{};
    std::string m_interface{};
    std::recursive_mutex m_stateMutex;
    std::unordered_map<int, int64_t> m_releaseTimes;
    std::unordered_map<std::string, int64_t> m_wifiLastSeen;
    std::set<std::string> m_forwardedAddresses;
    std::string m_chain;
    bool m_firewallReady{};
    bool m_ownsFirewall{};


  public:
    explicit RlsUdpTask(TaskBase *base, uint64_t sti, Vector3 phyLocation, bool m_wifi, std::string m_ueInterface, std::string m_interface);
    ~RlsUdpTask() override = default;

  protected:
    void onStart() override;
    void onLoop() override;
    void onQuit() override;

  private:
    void receiveRlsPdu(const InetAddress &addr, std::unique_ptr<rls::RlsMessage> &&msg);
    void sendRlsPdu(const InetAddress &addr, const rls::RlsMessage &msg);
    void heartbeatCycle(int64_t time);
    void setWifiForwarding(const std::string &address, bool enabled);
    void clearWifiForwarding();
  public:
    void initialize(NtsTask *ctlTask);
    void send(int ueId, const rls::RlsMessage &msg);
};

} // namespace nr::gnb
