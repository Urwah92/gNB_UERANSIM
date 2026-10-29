//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include "task.hpp"

#include <gnb/gtp/proto.hpp>
#include <gnb/rls/task.hpp>
#include <utils/constants.hpp>
#include <utils/libc_error.hpp>
#include <gnb/ngap/task.hpp>


#include <asn/ngap/ASN_NGAP_QosFlowSetupRequestItem.h>
#include <cstdlib>
#include <string>

namespace nr::gnb
{

GtpTask::GtpTask(TaskBase *base)
    : m_base{base}, m_udpServer{}, m_ueContexts{}, m_rateLimiter(std::make_unique<RateLimiter>()), m_pduSessions{},
      m_sessionTree{}
{
    m_logger = m_base->logBase->makeUniqueLogger("gtp");
}

void GtpTask::onStart()
{
    try
    {
        m_udpServer = new udp::UdpServerTask(m_base->config->gtpIp, cons::GtpPort, this);
        m_udpServer->start();
    }
    catch (const LibError &e)
    {
        m_logger->err("GTP/UDP task could not be created. %s", e.what());
    }
}

void GtpTask::onQuit()
{
    if (m_udpServer)
    {
        m_udpServer->quit();
        delete m_udpServer;
        m_udpServer = nullptr;
    }

    m_ueContexts.clear();
}

void GtpTask::onLoop()
{
    auto msg = take();
    if (!msg)
        return;

    switch (msg->msgType)
    {
    case NtsMessageType::GNB_NGAP_TO_GTP: {
        auto &w = dynamic_cast<NmGnbNgapToGtp &>(*msg);
        switch (w.present)
        {
        case NmGnbNgapToGtp::UE_CONTEXT_UPDATE: {
            handleUeContextUpdate(*w.update);
            break;
        }
        case NmGnbNgapToGtp::UE_CONTEXT_RELEASE: {
            handleUeContextDelete(w.ueId);
            break;
        }
        case NmGnbNgapToGtp::SESSION_CREATE: {
            handleSessionCreate(w.resource);
            break;
        }
        case NmGnbNgapToGtp::SESSION_RELEASE: {
            handleSessionRelease(w.ueId, w.psi);
            break;
        }
        }
        break;
    }
    case NtsMessageType::GNB_RLS_TO_GTP: {
        auto &w = dynamic_cast<NmGnbRlsToGtp &>(*msg);
        switch (w.present)
        {
            case NmGnbRlsToGtp::DATA_PDU_DELIVERY: {
                handleUplinkData(w.ueId, w.psi, std::move(w.pdu));
                break;
            }
            case NmGnbRlsToGtp::DATA_PDU_RELEASE: {
                // Export an independent context. The source tunnel remains live until target confirmation.
                std::vector<uint64_t> sessions;
                if (w.psi == 0)
                    m_sessionTree.enumerateByUe(w.ueId, sessions);
                else
                    sessions.push_back(MakeSessionResInd(w.ueId, w.psi));
                for (const auto key : sessions)
                {
                    auto found = m_pduSessions.find(key);
                    if (found == m_pduSessions.end() || !found->second || !found->second->qosFlows)
                        continue;
                    const auto &source = *found->second;
                    auto copy = std::make_unique<PduSessionResource>(source.ueId, source.psi);
                    copy->sessionAmbr = source.sessionAmbr;
                    copy->dataForwardingNotPossible = source.dataForwardingNotPossible;
                    copy->sessionType = source.sessionType;
                    copy->upTunnel.teid = source.upTunnel.teid;
                    copy->upTunnel.address = source.upTunnel.address.copy();
                    copy->downTunnel.teid = source.downTunnel.teid;
                    copy->downTunnel.address = source.downTunnel.address.copy();
                    // Relayed sessions carry accepted QFIs, not full QoS setup descriptors.
                    // Copy those identifiers directly so a second handover can export an imported session too.
                    copy->qosFlows = asn::WrapUnique(asn::New<ASN_NGAP_QosFlowSetupRequestList>(),
                                                     asn_DEF_ASN_NGAP_QosFlowSetupRequestList);
                    for (int i = 0; i < source.qosFlows->list.count; ++i)
                    {
                        const auto *sourceFlow = source.qosFlows->list.array[i];
                        if (!sourceFlow)
                            continue;
                        auto *flow = asn::New<ASN_NGAP_QosFlowSetupRequestItem>();
                        flow->qosFlowIdentifier = sourceFlow->qosFlowIdentifier;
                        asn::SequenceAdd(*copy->qosFlows, flow);
                    }
                    auto exported = std::make_unique<NmGnbGtpToNgap>(NmGnbGtpToNgap::DATA_PDU_INFO);
                    exported->ueId = source.ueId;
                    exported->psi = source.psi;
                    exported->m_pduSession = std::move(copy);
                    m_base->ngapTask->push(std::move(exported));
                }
                break;
            }
        }
        break;
    }
    case NtsMessageType::UDP_SERVER_RECEIVE:
        handleUdpReceive(dynamic_cast<udp::NwUdpServerReceive &>(*msg));
        break;
    default:
        m_logger->unhandledNts(*msg);
        break;
    }
}

void GtpTask::handleUeContextUpdate(const GtpUeContextUpdate &msg)
{
    if (!m_ueContexts.count(msg.ueId))
        m_ueContexts[msg.ueId] = std::make_unique<GtpUeContext>(msg.ueId);

    auto &ue = m_ueContexts[msg.ueId];
    ue->ueAmbr = msg.ueAmbr;

    updateAmbrForUe(ue->ueId);
}

void GtpTask::handleSessionCreate(PduSessionResource *resource)
{
    std::unique_ptr<PduSessionResource> session(resource);
    if (!session || !m_ueContexts.count(session->ueId))
    {
        m_logger->err("PDU session resource cannot be created without UE context");
        return;
    }
    const auto key = MakeSessionResInd(session->ueId, session->psi);
    const auto old = m_pduSessions.find(key);
    if (old != m_pduSessions.end() && old->second)
        m_sessionTree.remove(key, old->second->downTunnel.teid);
    const auto teid = session->downTunnel.teid;
    const auto ueId = session->ueId;
    m_pduSessions[key] = std::move(session);
    m_sessionTree.insert(key, teid);
    updateAmbrForUe(ueId);
    updateAmbrForSession(key);
}

void GtpTask::handleSessionRelease(int ueId, int psi)
{
    const auto key = MakeSessionResInd(ueId, psi);
    const auto found = m_pduSessions.find(key);
    if (found == m_pduSessions.end())
        return;
    m_rateLimiter->updateSessionUplinkLimit(key, 0);
    m_rateLimiter->updateSessionDownlinkLimit(key, 0);
    if (found->second)
        m_sessionTree.remove(key, found->second->downTunnel.teid);
    m_pduSessions.erase(found);
}

void GtpTask::handleUeContextDelete(int ueId)
{
    std::vector<uint64_t> sessions;
    m_sessionTree.enumerateByUe(ueId, sessions);
    for (const auto key : sessions)
        handleSessionRelease(ueId, GetPsi(key));
    m_rateLimiter->updateUeUplinkLimit(ueId, 0);
    m_rateLimiter->updateUeDownlinkLimit(ueId, 0);
    m_ueContexts.erase(ueId);
}

void GtpTask::handleUplinkData(int ueId, int psi, OctetString &&pdu)
{
    if (pdu.length() < 20)
        return;
    const uint8_t *data = pdu.data();
    if ((data[0] >> 4 & 0xF) != 4)
        {
            return;
        }


    uint64_t sessionInd = MakeSessionResInd(ueId, psi);

    if (!m_pduSessions.count(sessionInd))
    {
        m_logger->err("Uplink data failure, PDU session not found. UE[%d] PSI[%d]", ueId, psi);
        return;
    }

    auto &pduSession = m_pduSessions[sessionInd];
    if (m_rateLimiter->allowUplinkPacket(sessionInd, static_cast<int64_t>(pdu.length())))
    {
        // TODO: currently using first QSI
        gtp::GtpMessage gtp{};
        gtp.payload = std::move(pdu);
        gtp.msgType = gtp::GtpMessage::MT_G_PDU;
        gtp.teid = pduSession->upTunnel.teid;
        auto ul = std::make_unique<gtp::UlPduSessionInformation>();
        ul->qfi = static_cast<int>(pduSession->qosFlows->list.array[0]->qosFlowIdentifier);
        auto cont = std::make_unique<gtp::PduSessionContainerExtHeader>();
        cont->pduSessionInformation = std::move(ul);
        gtp.extHeaders.push_back(std::move(cont));

        OctetString gtpPdu;
        if (!gtp::EncodeGtpMessage(gtp, gtpPdu))
            m_logger->err("Uplink data failure, GTP encoding failed");
        else
            {
                m_udpServer->send(InetAddress(pduSession->upTunnel.address, cons::GtpPort), gtpPdu);
            }
    }
}

void GtpTask::handleUdpReceive(const udp::NwUdpServerReceive &msg)
{
    OctetView buffer{msg.packet};
    auto gtp = gtp::DecodeGtpMessage(buffer);

    if (!gtp)
        return;

    switch (gtp->msgType)
    {
    case gtp::GtpMessage::MT_G_PDU: {
        auto sessionInd = m_sessionTree.findByDownTeid(gtp->teid);
        if (sessionInd == 0)
        {
            m_logger->err("TEID %d not found on GTP-U Downlink", gtp->teid);
            return;
        }

        if (m_rateLimiter->allowDownlinkPacket(sessionInd, gtp->payload.length()))
        {
            auto w = std::make_unique<NmGnbGtpToRls>(NmGnbGtpToRls::DATA_PDU_DELIVERY);
            w->ueId = GetUeId(sessionInd);
            w->psi = GetPsi(sessionInd);
            w->pdu = std::move(gtp->payload);
            m_base->rlsTask->push(std::move(w));
        }
        return;
    }
    case gtp::GtpMessage::MT_ECHO_REQUEST: {
        gtp::GtpMessage gtpResponse{};
        gtpResponse.msgType = gtp::GtpMessage::MT_ECHO_RESPONSE;
        gtpResponse.seq = gtp->seq;
        gtpResponse.payload = OctetString::FromOctet2({14, 0});

        OctetString gtpPdu;
        if (gtp::EncodeGtpMessage(gtpResponse, gtpPdu))
            m_udpServer->send(msg.fromAddress, gtpPdu);
        else
            m_logger->err("Uplink data failure, GTP encoding failed");
        return;
    }
    default: {
        m_logger->err("Unhandled GTP-U message type: %d", gtp->msgType);
        return;
    }
    }
}

void GtpTask::updateAmbrForUe(int ueId)
{
    if (!m_ueContexts.count(ueId))
        {
            return;
        }
    auto &ue = m_ueContexts[ueId];
    m_rateLimiter->updateUeUplinkLimit(ueId, ue->ueAmbr.ulAmbr);
    m_rateLimiter->updateUeDownlinkLimit(ueId, ue->ueAmbr.dlAmbr);
}

void GtpTask::updateAmbrForSession(uint64_t pduSession)
{
    if (!m_pduSessions.count(pduSession))
        {
            return;
        }
    auto &sess = m_pduSessions[pduSession];
    m_rateLimiter->updateSessionUplinkLimit(pduSession, sess->sessionAmbr.ulAmbr);
    m_rateLimiter->updateSessionDownlinkLimit(pduSession, sess->sessionAmbr.dlAmbr);
}

} // namespace nr::gnb
