//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include "task.hpp"
#include <gnb/rls/udp_task.hpp>
#include <gnb/gtp/task.hpp>
#include <gnb/rrc/task.hpp>
#include <gnb/ngap/task.hpp>
#include <utils/common.hpp>
#include <utils/random.hpp>
#include <cstring>
#include <unistd.h>

namespace nr::gnb
{

GnbRlsTask::GnbRlsTask(TaskBase *base) : m_base{base}
{
    m_logger = m_base->logBase->makeUniqueLogger("rls");
    m_sti = Random::Mixed(base->config->name).nextUL();
    m_udpTask = new RlsUdpTask(base, m_sti, base->config->phyLocation, base->config->wifi, base->config->ueInterface, base->config->interface);
    m_ctlTask = new RlsControlTask(base, m_sti);

    m_udpTask->initialize(m_ctlTask);
    m_ctlTask->initialize(this, m_udpTask);
}

void GnbRlsTask::onStart()
{
    m_udpTask->start();
    m_ctlTask->start();

}

void GnbRlsTask::onLoop()
{
    auto msg = take();
    if (!msg)
        return;

    switch (msg->msgType)
    {
    case NtsMessageType::GNB_RLS_TO_RLS: {
        auto &w = dynamic_cast<NmGnbRlsToRls &>(*msg);
        switch (w.present)
        {
        case NmGnbRlsToRls::SIGNAL_DETECTED: {
            auto m = std::make_unique<NmGnbRlsToRrc>(NmGnbRlsToRrc::SIGNAL_DETECTED);
            m->ueId = w.ueId;
            m_base->rrcTask->push(std::move(m));
            break;
        }
        case NmGnbRlsToRls::SIGNAL_LOST: {
            // Weak-signal heartbeats remain live. This event means the peer actually stopped heartbeating.
            m_logger->debug("UE[%d] signal lost", w.ueId);
            if (!m_base->config->wifi)
            {
                auto lost = std::make_unique<NmGnbRlsToRrc>(NmGnbRlsToRrc::SIGNAL_LOST);
                lost->ueId = w.ueId;
                lost->psi = 0;
                m_base->rrcTask->push(std::move(lost));
            }
            break;
        }
        case NmGnbRlsToRls::SESSION_RELEASE: {
            auto release = std::make_unique<NmGnbRlsToNgap>(NmGnbRlsToNgap::SESSION_RELEASE);
            release->ueId = w.ueId;
            release->psi = w.psi;
            m_base->ngapTask->push(std::move(release));
            break;
        }
        case NmGnbRlsToRls::UPLINK_DATA: {
            auto m = std::make_unique<NmGnbRlsToGtp>(NmGnbRlsToGtp::DATA_PDU_DELIVERY);
            m->ueId = w.ueId;
            m->psi = w.psi;
            m->pdu = std::move(w.data);
            m_base->gtpTask->push(std::move(m));
            break;
        }
        case NmGnbRlsToRls::UPLINK_RRC: {
            auto m = std::make_unique<NmGnbRlsToRrc>(NmGnbRlsToRrc::UPLINK_RRC);
            m->ueId = w.ueId;
            m->rrcChannel = w.rrcChannel;
            m->data = std::move(w.data);
            m_base->rrcTask->push(std::move(m));
            break;
        }
        case NmGnbRlsToRls::RADIO_LINK_FAILURE: {
            m_logger->debug("radio link failure [%d]", (int)w.rlfCause);
            break;
        }
        case NmGnbRlsToRls::TRANSMISSION_FAILURE: {
            m_logger->debug("transmission failure [%s]", "");
            break;
        }
        case NmGnbRlsToRls::SESSION_CHANGE:{
            auto m = std::make_unique<NmGnbRlsToGtp>(NmGnbRlsToGtp::DATA_PDU_RELEASE);
            m->ueId = w.ueId;
            m->psi = w.psi;
            m_base->gtpTask->push(std::move(m));
            break;
        }
        case NmGnbRlsToRls::SESSION_TRANSMISSION: {
            auto m = std::make_unique<NmGnbRlsToNgap>(NmGnbRlsToNgap::PACKET_SWITCH_REQUEST);
            m->ueId = w.ueId;
            m->psi = w.psi;
            m->amfId = w.amfId;
            m->m_pduSession = std::move(w.m_pduSession);
            m->m_ueSecurityCapability = std::move(w.m_ueSecurityCapability);
            m_base->ngapTask->push(std::move(m));
            break;
        }
        default: {
            m_logger->unhandledNts(*msg);
            break;
        }
        }
        break;
    }
    case NtsMessageType::GNB_RRC_TO_RLS: {
        auto &w = dynamic_cast<NmGnbRrcToRls &>(*msg);
        switch (w.present)
        {
        case NmGnbRrcToRls::RRC_PDU_DELIVERY: {
            auto m = std::make_unique<NmGnbRlsToRls>(NmGnbRlsToRls::DOWNLINK_RRC);
            m->ueId = w.ueId;
            m->rrcChannel = w.channel;
            m->pduId = 0;
            m->data = std::move(w.pdu);
            m_ctlTask->push(std::move(m));
            break;
        }
        }
        break;
    }
    case NtsMessageType::GNB_GTP_TO_RLS: {
        auto &w = dynamic_cast<NmGnbGtpToRls &>(*msg);
        switch (w.present)
        {
        case NmGnbGtpToRls::DATA_PDU_DELIVERY: {
            auto m = std::make_unique<NmGnbRlsToRls>(NmGnbRlsToRls::DOWNLINK_DATA);
            m->ueId = w.ueId;
            m->psi = w.psi;
            m->data = std::move(w.pdu);
            m_ctlTask->push(std::move(m));
            break;
        }
        }
        break;
    }
    case NtsMessageType::GNB_NGAP_TO_RLS: {
        auto &w = dynamic_cast<NmGnbNgapToRls &>(*msg);
        switch (w.present)
        {
        case NmGnbNgapToRls::DATA_PDU_INFO: {
            auto m = std::make_unique<NmGnbRlsToRls>(NmGnbRlsToRls::DOWNLINK_SESSION);
            m->ueId = w.ueId;
            m->psi = w.psi;
            m->amfId = w.amfId;
            m->m_pduSession = std::move(w.m_pduSession);
            m_ctlTask->push(std::move(m));
            break;
        }
        case NmGnbNgapToRls::XN_SESSION_CREATE:{
            auto l = std::make_unique<NmGnbRlsToRls>(NmGnbRlsToRls::DOWNLINK_XN_DATA);
            l->ueId = w.ueId;
            l->psi = w.psi;
            m_ctlTask->push(std::move(l));
            break;
            }
        }
        break;
    }
    default:
        m_logger->unhandledNts(*msg);
        break;
    }
}

void GnbRlsTask::onQuit()
{
    m_ctlTask->quit();
    m_udpTask->quit();
    delete m_udpTask;
    delete m_ctlTask;
}

} // namespace nr::gnb
