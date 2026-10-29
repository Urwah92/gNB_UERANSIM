//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#pragma once

#include <optional>
#include <unordered_map>

#include <gnb/nts.hpp>
#include <gnb/types.hpp>
#include <lib/app/monitor.hpp>
#include <utils/logger.hpp>
#include <utils/nts.hpp>
//Urwah
#include <lib/nas/ie4.hpp>
#include <asn/ngap/ASN_NGAP_PDUSessionResourceToBeSwitchedDLList.h>

extern "C"
{
    struct ASN_NGAP_NGAP_PDU;
    struct ASN_NGAP_NGSetupResponse;
    struct ASN_NGAP_NGSetupFailure;
    struct ASN_NGAP_ErrorIndication;
    struct ASN_NGAP_DownlinkNASTransport;
    struct ASN_NGAP_RerouteNASRequest;
    struct ASN_NGAP_PDUSessionResourceSetupRequest;
    struct ASN_NGAP_InitialContextSetupRequest;
    struct ASN_NGAP_UEContextReleaseCommand;
    struct ASN_NGAP_UEContextModificationRequest;
    struct ASN_NGAP_AMFConfigurationUpdate;
    struct ASN_NGAP_OverloadStart;
    struct ASN_NGAP_OverloadStop;
    struct ASN_NGAP_PDUSessionResourceReleaseCommand;
    struct ASN_NGAP_Paging;
    struct ASN_NGAP_PathSwitchRequestAcknowledge;
    struct ASN_NGAP_PathSwitchRequestFailure;
}

namespace nr::gnb
{

class SctpTask;
class GnbRrcTask;
class GtpTask;
class GnbAppTask;

class NgapTask : public NtsTask
{
  private:
    TaskBase *m_base;
    std::unique_ptr<Logger> m_logger;

    std::unordered_map<int, NgapAmfContext *> m_amfCtx;
    std::unordered_map<int, NgapUeContext *> m_ueCtx;
    int64_t m_ueNgapIdCounter;
    uint32_t m_downlinkTeidCounter;
    bool m_isInitialized;
    struct PendingPathSwitch
    {
        std::unique_ptr<PduSessionResource> resource;
        int amfId;
        int64_t startedAt;
    };
    // The response identifies a UE by NGAP IDs and each session by PSI.
    std::unordered_map<uint64_t, PendingPathSwitch> m_pendingPathSwitches;

    friend class GnbCmdHandler;
    friend struct GnbHandoverTest;

  public:
    explicit NgapTask(TaskBase *base);
    ~NgapTask() override = default;


  protected:
    void onStart() override;
    void onLoop() override;
    void onQuit() override;

  private:
    /* Utility functions */
    void createAmfContext(const GnbAmfConfig &config);
    NgapAmfContext *findAmfContext(int ctxId);
    void createUeContext(int ueId);
    NgapUeContext *findUeContext(int ctxId);
    NgapUeContext *findUeByRanId(int64_t ranUeNgapId);
    NgapUeContext *findUeByAmfId(int64_t amfUeNgapId);
    NgapUeContext *findUeByNgapIdPair(int amfCtxId, const NgapIdPair &idPair);
    void deleteUeContext(int ueId);
    void deleteAmfContext(int amfId);

    /* Interface management */
    void handleAssociationSetup(int amfId, int ascId, int inCount, int outCount);
    void handleAssociationShutdown(int amfId);
    void sendNgSetupRequest(int amfId);
    void sendErrorIndication(int amfId, NgapCause cause = NgapCause::Protocol_unspecified, int ueId = 0);
    void receiveNgSetupResponse(int amfId, ASN_NGAP_NGSetupResponse *msg);
    //Urwah
    void receivePSRAck(int amfId, ASN_NGAP_PathSwitchRequestAcknowledge *msg);
    void receivePathSwitchFailure(int amfId, ASN_NGAP_PathSwitchRequestFailure *msg);
    void expirePathSwitches();

    void receiveNgSetupFailure(int amfId, ASN_NGAP_NGSetupFailure *msg);
    void receiveErrorIndication(int amfId, ASN_NGAP_ErrorIndication *msg);
    void receiveAmfConfigurationUpdate(int amfId, ASN_NGAP_AMFConfigurationUpdate *msg);
    void receiveOverloadStart(int amfId, ASN_NGAP_OverloadStart *msg);
    void receiveOverloadStop(int amfId, ASN_NGAP_OverloadStop *msg);

    /* Message transport */
    void sendNgapNonUe(int amfId, ASN_NGAP_NGAP_PDU *pdu);
    void sendNgapUeAssociated(int ueId, ASN_NGAP_NGAP_PDU *pdu);
    //Urwah
    bool sendNgapUeAssociatedPathSwitchReq(int ueId, ASN_NGAP_NGAP_PDU *pdu, const PduSessionResource& pduSessionResource,
                const nas::IEUeSecurityCapability ueSecurityCapability);
    //ASN_NGAP_PDUSessionResourceToBeSwitchedDLList PDUSessionResourceToBeSwitchedDLList(const PduSessionResource& pduSessionResource);


    void handleSctpMessage(int amfId, uint16_t stream, const UniqueBuffer &buffer);
    bool handleSctpStreamId(int amfId, int stream, const ASN_NGAP_NGAP_PDU &pdu);

    /* NAS transport */
    void handleInitialNasTransport(int ueId, const OctetString &nasPdu, int64_t rrcEstablishmentCause,
                                   const std::optional<GutiMobileIdentity> &sTmsi);
    void handleUplinkNasTransport(int ueId, const OctetString &nasPdu);
    void receiveDownlinkNasTransport(int amfId, ASN_NGAP_DownlinkNASTransport *msg);
    void deliverDownlinkNas(int ueId, OctetString &&nasPdu);
    void sendNasNonDeliveryIndication(int ueId, const OctetString &nasPdu, NgapCause cause);
    void receiveRerouteNasRequest(int amfId, ASN_NGAP_RerouteNASRequest *msg);
    //Urwah
    void handlePathSwitchRequest(int ueId, int64_t sourceAmfUeId, std::unique_ptr<PduSessionResource> resource,
                                 const nas::IEUeSecurityCapability &ueSecurityCapability);
/* PDU session management */
    void receiveSessionResourceSetupRequest(int amfId, ASN_NGAP_PDUSessionResourceSetupRequest *msg);
    void receiveSessionResourceReleaseCommand(int amfId, ASN_NGAP_PDUSessionResourceReleaseCommand *msg);
    std::optional<NgapCause> setupPduSessionResource(NgapUeContext *ue, PduSessionResource *resource);

    /* UE context management */
    void receiveInitialContextSetup(int amfId, ASN_NGAP_InitialContextSetupRequest *msg);
    void receiveContextRelease(int amfId, ASN_NGAP_UEContextReleaseCommand *msg);
    void receiveContextModification(int amfId, ASN_NGAP_UEContextModificationRequest *msg);
    void sendContextRelease(int ueId, NgapCause cause);

    /* NAS Node Selection */
    NgapAmfContext *selectAmf(int ueId);
    NgapAmfContext *selectNewAmfForReAllocation(int ueId, int initiatedAmfId, int amfSetId);

    /* Radio resource control */
    void handleRadioLinkFailure(int ueId);
    void receivePaging(int amfId, ASN_NGAP_Paging *msg);
    //Urwah
    void handleSignalLost(int ueId, int psi);
};

} // namespace nr::gnb
