//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include "encode.hpp"
#include "task.hpp"
#include "utils.hpp"

#include <gnb/gtp/task.hpp>
#include <gnb/rrc/task.hpp>

#include <asn/ngap/ASN_NGAP_FiveG-S-TMSI.h>
#include <asn/ngap/ASN_NGAP_Paging.h>


namespace nr::gnb
{

void NgapTask::handleRadioLinkFailure(int ueId)
{
    if (!m_ueCtx.count(ueId))
        return;
    for (auto it = m_pendingPathSwitches.begin(); it != m_pendingPathSwitches.end();)
        if (it->second.resource->ueId == ueId)
            it = m_pendingPathSwitches.erase(it);
        else
            ++it;
    // Notify GTP task
    auto w = std::make_unique<NmGnbNgapToGtp>(NmGnbNgapToGtp::UE_CONTEXT_RELEASE);
    w->ueId = ueId;
    m_base->gtpTask->push(std::move(w));
    // Notify AMF
    sendContextRelease(ueId, NgapCause::RadioNetwork_radio_connection_with_ue_lost);
}

void NgapTask::receivePaging(int amfId, ASN_NGAP_Paging *msg)
{
    m_logger->debug("Paging received");

    auto *amf = findAmfContext(amfId);
    if (amf == nullptr)
        return;

    auto *ieUePagingIdentity = asn::ngap::GetProtocolIe(msg, ASN_NGAP_ProtocolIE_ID_id_UEPagingIdentity);
    auto *ieTaiListForPaging = asn::ngap::GetProtocolIe(msg, ASN_NGAP_ProtocolIE_ID_id_TAIListForPaging);

    if (ieUePagingIdentity == nullptr || ieTaiListForPaging == nullptr ||
        ieUePagingIdentity->UEPagingIdentity.present != ASN_NGAP_UEPagingIdentity_PR_fiveG_S_TMSI)
    {
        m_logger->err("Invalid parameters received in Paging message");
        return;
    }

    auto w = std::make_unique<NmGnbNgapToRrc>(NmGnbNgapToRrc::PAGING);
    w->uePagingTmsi =
        asn::UniqueCopy(*ieUePagingIdentity->UEPagingIdentity.choice.fiveG_S_TMSI, asn_DEF_ASN_NGAP_FiveG_S_TMSI);
    w->taiListForPaging = asn::UniqueCopy(ieTaiListForPaging->TAIListForPaging, asn_DEF_ASN_NGAP_TAIListForPaging);

    m_base->rrcTask->push(std::move(w));
}

//Urwah
void NgapTask::handleSignalLost(int ueId, int psi)
{
    if (!m_ueCtx.count(ueId))
        return;
    if (psi == 0)
    {
        handleRadioLinkFailure(ueId);
        return;
    }
    auto *ue = findUeContext(ueId);
    if (!ue || psi < 1 || psi > 15 || !ue->pduSessions.erase(psi))
        return;
    auto release = std::make_unique<NmGnbNgapToGtp>(NmGnbNgapToGtp::SESSION_RELEASE);
    release->ueId = ueId;
    release->psi = psi;
    m_base->gtpTask->push(std::move(release));
    m_logger->info("Released source handover session UE[%d] PSI[%d]", ueId, psi);
    if (ue->pduSessions.empty())
    {
        // This is local handover cleanup, not a request to release the target's AMF context.
        auto context = std::make_unique<NmGnbNgapToGtp>(NmGnbNgapToGtp::UE_CONTEXT_RELEASE);
        context->ueId = ueId;
        m_base->gtpTask->push(std::move(context));
        if (m_base->rrcTask)
        {
            auto rrc = std::make_unique<NmGnbNgapToRrc>(NmGnbNgapToRrc::HANDOVER_RELEASE);
            rrc->ueId = ueId;
            m_base->rrcTask->push(std::move(rrc));
        }
        deleteUeContext(ueId);
    }
}

} // namespace nr::gnb
