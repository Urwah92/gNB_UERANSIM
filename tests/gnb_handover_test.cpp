#include <gnb/ngap/task.hpp>
#include <gnb/ngap/encode.hpp>
#include <gnb/ngap/utils.hpp>
#include <gnb/gtp/task.hpp>
#include <gnb/sctp/task.hpp>
#include <asn/ngap/ASN_NGAP_NGAP-PDU.h>
#include <asn/ngap/ASN_NGAP_PathSwitchRequestAcknowledge.h>
#include <asn/ngap/ASN_NGAP_PathSwitchRequestFailure.h>
#include <asn/ngap/ASN_NGAP_PathSwitchRequestAcknowledgeTransfer.h>
#include <asn/ngap/ASN_NGAP_PDUSessionResourceSwitchedItem.h>
#include <asn/ngap/ASN_NGAP_PDUSessionResourceReleasedItemPSFail.h>
#include <asn/ngap/ASN_NGAP_QosFlowSetupRequestItem.h>
#include <asn/ngap/ASN_NGAP_NonDynamic5QIDescriptor.h>
#include <asn/ngap/ASN_NGAP_UPTransportLayerInformation.h>
#include <asn/ngap/ASN_NGAP_GTPTunnel.h>
#include <iostream>
#include <stdexcept>

namespace nr::gnb
{
struct GnbHandoverTest
{
    static void require(bool condition, const char *message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
    static uint64_t key(int ue, int psi) { return MakeSessionResInd(ue, psi); }

    static std::unique_ptr<PduSessionResource> resource(int ue, int psi, bool relayed = false)
    {
        auto result = std::make_unique<PduSessionResource>(ue, psi);
        result->sessionType = PduSessionType::IPv4;
        result->sessionAmbr = {1000000, 1000000};
        result->upTunnel.teid = 77;
        result->upTunnel.address = utils::IpToOctetString("127.0.0.7");
        result->downTunnel.teid = static_cast<uint32_t>(100 + psi + 16 * ue);
        result->downTunnel.address = utils::IpToOctetString("127.0.0.8");
        result->qosFlows = asn::WrapUnique(asn::New<ASN_NGAP_QosFlowSetupRequestList>(),
                                         asn_DEF_ASN_NGAP_QosFlowSetupRequestList);
        for (int qfi : {1, 7})
        {
            auto *flow = asn::New<ASN_NGAP_QosFlowSetupRequestItem>();
            flow->qosFlowIdentifier = qfi;
            if (relayed)
            {
                asn::SequenceAdd(*result->qosFlows, flow);
                continue;
            }
            auto &params = flow->qosFlowLevelQosParameters;
            params.qosCharacteristics.present = ASN_NGAP_QosCharacteristics_PR_nonDynamic5QI;
            params.qosCharacteristics.choice.nonDynamic5QI = asn::New<ASN_NGAP_NonDynamic5QIDescriptor>();
            params.qosCharacteristics.choice.nonDynamic5QI->fiveQI = 9;
            params.allocationAndRetentionPriority.priorityLevelARP = 8;
            asn::SequenceAdd(*result->qosFlows, flow);
        }
        return result;
    }

    static void ue(NgapTask &ngap, int id, int64_t amfUeId)
    {
        auto *context = new NgapUeContext(id);
        context->associatedAmfId = 1;
        context->ranUeNgapId = id + 100;
        context->amfUeNgapId = amfUeId;
        ngap.m_ueCtx[id] = context;
    }

    static asn::Unique<ASN_NGAP_PathSwitchRequestAcknowledge> ack(int ue, int64_t amfUeId, int psi,
                                                               uint32_t uplinkTeid = 555)
    {
        auto result = asn::WrapUnique(asn::New<ASN_NGAP_PathSwitchRequestAcknowledge>(),
                                      asn_DEF_ASN_NGAP_PathSwitchRequestAcknowledge);
        auto *amf = asn::New<ASN_NGAP_PathSwitchRequestAcknowledgeIEs>();
        amf->id = ASN_NGAP_ProtocolIE_ID_id_AMF_UE_NGAP_ID;
        amf->value.present = ASN_NGAP_PathSwitchRequestAcknowledgeIEs__value_PR_AMF_UE_NGAP_ID;
        asn::SetSigned64(amfUeId, amf->value.choice.AMF_UE_NGAP_ID);
        asn::ngap::AddProtocolIe(*result, amf);
        auto *ran = asn::New<ASN_NGAP_PathSwitchRequestAcknowledgeIEs>();
        ran->id = ASN_NGAP_ProtocolIE_ID_id_RAN_UE_NGAP_ID;
        ran->value.present = ASN_NGAP_PathSwitchRequestAcknowledgeIEs__value_PR_RAN_UE_NGAP_ID;
        ran->value.choice.RAN_UE_NGAP_ID = ue + 100;
        asn::ngap::AddProtocolIe(*result, ran);
        auto transfer = asn::WrapUnique(asn::New<ASN_NGAP_PathSwitchRequestAcknowledgeTransfer>(),
                                        asn_DEF_ASN_NGAP_PathSwitchRequestAcknowledgeTransfer);
        transfer->uL_NGU_UP_TNLInformation = asn::New<ASN_NGAP_UPTransportLayerInformation>();
        transfer->uL_NGU_UP_TNLInformation->present = ASN_NGAP_UPTransportLayerInformation_PR_gTPTunnel;
        auto *tunnel = asn::New<ASN_NGAP_GTPTunnel>();
        transfer->uL_NGU_UP_TNLInformation->choice.gTPTunnel = tunnel;
        asn::SetOctetString4(tunnel->gTP_TEID, octet4{uplinkTeid});
        asn::SetBitString(tunnel->transportLayerAddress, utils::IpToOctetString("127.0.0.9"));
        auto encoded = ngap_encode::EncodeS(asn_DEF_ASN_NGAP_PathSwitchRequestAcknowledgeTransfer, transfer.get());
        require(encoded.length() > 0, "test ACK tunnel transfer must encode");
        auto *sessions = asn::New<ASN_NGAP_PathSwitchRequestAcknowledgeIEs>();
        sessions->id = ASN_NGAP_ProtocolIE_ID_id_PDUSessionResourceSwitchedList;
        sessions->value.present = ASN_NGAP_PathSwitchRequestAcknowledgeIEs__value_PR_PDUSessionResourceSwitchedList;
        auto *item = asn::New<ASN_NGAP_PDUSessionResourceSwitchedItem>();
        item->pDUSessionID = psi;
        asn::SetOctetString(item->pathSwitchRequestAcknowledgeTransfer, encoded);
        asn::SequenceAdd(sessions->value.choice.PDUSessionResourceSwitchedList, item);
        asn::ngap::AddProtocolIe(*result, sessions);
        return result;
    }

    static void run()
    {
        GnbConfig config{};
        config.plmn = {208, 93, false};
        config.nci = 16;
        config.tac = 1;
        config.gtpIp = "127.0.0.8";
        LogBase log("gnb-handover-test.log");
        TaskBase base{};
        base.config = &config;
        base.logBase = &log;
        GtpTask gtp(&base);
        NgapTask ngap(&base);
        SctpTask sctp(&base);
        base.gtpTask = &gtp;
        base.ngapTask = &ngap;
        base.sctpTask = &sctp;
        auto *amf = new NgapAmfContext();
        amf->ctxId = 1;
        amf->state = EAmfState::CONNECTED;
        amf->association.outStreams = 4;
        ngap.m_amfCtx[1] = amf;
        constexpr int64_t highAmfId = 0x123456789aLL;
        ue(ngap, 1, highAmfId);
        ue(ngap, 2, 22);
        nas::IEUeSecurityCapability capability;
        capability.b_128_5G_EA1 = -1;
        capability.b_128_5G_IA1 = -1;

        // Real request construction/ASN validation must retain three independent transactions.
        ngap.handlePathSwitchRequest(1, highAmfId, resource(1, 1), capability);
        ngap.handlePathSwitchRequest(1, highAmfId, resource(1, 2), capability);
        ngap.handlePathSwitchRequest(2, 22, resource(2, 1), capability);
        require(ngap.m_pendingPathSwitches.size() == 3, "three requests remain independently pending");
        auto first = ack(2, 22, 1, 222);
        ngap.receivePSRAck(1, first.get());
        require(ngap.m_pendingPathSwitches.size() == 2 && ngap.m_pendingPathSwitches.count(key(1, 1)),
                "out of order ACK must select its own UE and PSI");
        gtp.onLoop();
        gtp.onLoop();
        require(gtp.m_pduSessions.at(key(2, 1))->upTunnel.teid == 222, "ACK must update uplink TEID");
        require(gtp.m_pduSessions.at(key(2, 1))->upTunnel.address.get4UI(0) == 0x7f000009,
                "ACK must update uplink tunnel address");
        ngap.receivePSRAck(1, first.get());
        require(!gtp.poll(), "duplicate ACK must not recreate or release sessions");
        auto wrongAmf = ack(1, highAmfId, 1);
        ngap.receivePSRAck(2, wrongAmf.get());
        require(ngap.m_pendingPathSwitches.size() == 2, "wrong AMF association must not consume pending request");

        auto failure = asn::WrapUnique(asn::New<ASN_NGAP_PathSwitchRequestFailure>(),
                                       asn_DEF_ASN_NGAP_PathSwitchRequestFailure);
        auto *amfIe = asn::New<ASN_NGAP_PathSwitchRequestFailureIEs>();
        amfIe->id = ASN_NGAP_ProtocolIE_ID_id_AMF_UE_NGAP_ID;
        amfIe->value.present = ASN_NGAP_PathSwitchRequestFailureIEs__value_PR_AMF_UE_NGAP_ID;
        asn::SetSigned64(highAmfId, amfIe->value.choice.AMF_UE_NGAP_ID);
        asn::ngap::AddProtocolIe(*failure, amfIe);
        auto *ranIe = asn::New<ASN_NGAP_PathSwitchRequestFailureIEs>();
        ranIe->id = ASN_NGAP_ProtocolIE_ID_id_RAN_UE_NGAP_ID;
        ranIe->value.present = ASN_NGAP_PathSwitchRequestFailureIEs__value_PR_RAN_UE_NGAP_ID;
        ranIe->value.choice.RAN_UE_NGAP_ID = 101;
        asn::ngap::AddProtocolIe(*failure, ranIe);
        auto *released = asn::New<ASN_NGAP_PathSwitchRequestFailureIEs>();
        released->id = ASN_NGAP_ProtocolIE_ID_id_PDUSessionResourceReleasedListPSFail;
        released->value.present = ASN_NGAP_PathSwitchRequestFailureIEs__value_PR_PDUSessionResourceReleasedListPSFail;
        auto *failed = asn::New<ASN_NGAP_PDUSessionResourceReleasedItemPSFail>();
        failed->pDUSessionID = 2;
        asn::SequenceAdd(released->value.choice.PDUSessionResourceReleasedListPSFail, failed);
        asn::ngap::AddProtocolIe(*failure, released);
        ngap.receivePathSwitchFailure(1, failure.get());
        require(ngap.m_pendingPathSwitches.size() == 1 && ngap.m_pendingPathSwitches.count(key(1, 1)),
                "failure must leave other sessions pending");
        require(!gtp.poll(), "failure must not create target session");
        ngap.m_pendingPathSwitches.at(key(1, 1)).startedAt = utils::CurrentTimeMillis() - 10001;
        ngap.expirePathSwitches();
        require(ngap.m_pendingPathSwitches.empty(), "unanswered request must time out");
        require(!gtp.poll(), "timeout must not create or release a session");

        // Export every source session as a deep copy while retaining its original usable tunnel.
        GtpUeContextUpdate update(true, 1, {1000000, 1000000});
        gtp.handleUeContextUpdate(update);
        gtp.handleSessionCreate(resource(1, 1).release());
        gtp.handleSessionCreate(resource(1, 2, true).release());
        auto request = std::make_unique<NmGnbRlsToGtp>(NmGnbRlsToGtp::DATA_PDU_RELEASE);
        request->ueId = 1;
        request->psi = 0;
        gtp.push(std::move(request));
        gtp.onLoop();
        for (int i = 0; i < 2; ++i)
        {
            auto message = ngap.poll();
            require(message && message->msgType == NtsMessageType::GNB_GTP_TO_NGAP, "every source session exported");
            auto &exported = static_cast<NmGnbGtpToNgap &>(*message);
            const auto &original = gtp.m_pduSessions.at(key(1, exported.psi));
            require(exported.m_pduSession && original && exported.m_pduSession.get() != original.get(),
                    "source resource ownership must survive export");
            require(exported.m_pduSession->qosFlows->list.count == 2, "all QoS flows preserved in export");
            exported.m_pduSession->upTunnel.teid = 999;
            require(original->upTunnel.teid == 77, "export must not share mutable tunnel state");
        }
        ngap.m_ueCtx.at(1)->pduSessions = {1, 2};
        ngap.handleSignalLost(1, 1);
        gtp.onLoop();
        require(!gtp.m_pduSessions.count(key(1, 1)) && gtp.m_pduSessions.count(key(1, 2)),
                "confirmed handover must release only its source PSI");
        require(ngap.m_ueCtx.count(1), "other source sessions must retain UE context");
        ngap.handleSignalLost(1, 2);
        gtp.onLoop();
        gtp.onLoop();
        require(!ngap.m_ueCtx.count(1) && !gtp.m_ueContexts.count(1), "last PSI release removes local source context");
        ngap.handleSignalLost(1, 0);
        require(!gtp.poll(), "later source radio loss must not release the completed target AMF context");
        ngap.handleSignalLost(2, 0);
        gtp.onLoop();
        require(!gtp.m_ueContexts.count(2), "genuine heartbeat loss must release ordinary radio resources");
        ngap.onQuit();
        gtp.onQuit();
    }
};
} // namespace nr::gnb

int main()
{
    try
    {
        nr::gnb::GnbHandoverTest::run();
        std::cout << "gNB handover tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
