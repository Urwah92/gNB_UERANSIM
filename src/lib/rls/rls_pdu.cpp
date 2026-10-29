//
// This file is a part of UERANSIM project.
// Copyright (c) 2023 ALİ GÜNGÖR.
//
// https://github.com/aligungr/UERANSIM/
// See README, LICENSE, and CONTRIBUTING files for licensing details.
//

#include "rls_pdu.hpp"

#include <limits>
#include <stdexcept>

#include <asn/ngap/ASN_NGAP_QosFlowSetupRequestItem.h>
#include <utils/constants.hpp>

namespace rls
{
namespace
{
constexpr uint32_t MaxQosFlows = 64;
constexpr size_t MaxDatagramSize = 65507;

struct InvalidMessage
{
};

// OctetView is shared with other protocol decoders. Keep every RLS read bounded
// here, without changing the behavior of those unrelated decoders.
class Reader
{
    const OctetView &m_stream;

  public:
    explicit Reader(const OctetView &stream) : m_stream(stream)
    {
    }

    size_t remaining() const
    {
        return m_stream.remaining();
    }

    void require(size_t length) const
    {
        if (length > remaining())
            throw InvalidMessage{};
    }

    uint8_t byte() const
    {
        require(1);
        return static_cast<uint8_t>(m_stream.read());
    }

    uint32_t word() const
    {
        require(4);
        return m_stream.read4UI();
    }

    uint64_t longWord() const
    {
        require(8);
        return m_stream.read8UL();
    }

    OctetString bytes(size_t length) const
    {
        require(length);
        return m_stream.readOctetString(length);
    }

    OctetString address() const
    {
        auto length = word();
        if (length != 4 && length != 16)
            throw InvalidMessage{};
        return bytes(length);
    }

    void end() const
    {
        if (remaining() != 0)
            throw InvalidMessage{};
    }
};

void ValidateAddress(const OctetString &address)
{
    if (address.length() != 4 && address.length() != 16)
        throw std::invalid_argument("RLS tunnel address must contain 4 or 16 octets");
}

void EncodePayload(const RlsMessage &msg, OctetString &stream)
{
    if (msg.msgType == EMessageType::HEARTBEAT)
    {
        auto &m = static_cast<const RlsHeartBeat &>(msg);
        stream.appendOctet4(m.simPos.x);
        stream.appendOctet4(m.simPos.y);
        stream.appendOctet4(m.simPos.z);
    }
    else if (msg.msgType == EMessageType::HEARTBEAT_ACK)
    {
        auto &m = static_cast<const RlsHeartBeatAck &>(msg);
        stream.appendOctet4(m.dbm);
    }
    else if (msg.msgType == EMessageType::PDU_TRANSMISSION)
    {
        auto &m = static_cast<const RlsPduTransmission &>(msg);
        if (m.pduType != EPduType::RRC && m.pduType != EPduType::DATA)
            throw std::invalid_argument("Invalid RLS PDU type");
        stream.appendOctet(static_cast<uint8_t>(m.pduType));
        stream.appendOctet4(m.pduId);
        stream.appendOctet4(m.payload);
        stream.appendOctet4(m.pdu.length());
        stream.append(m.pdu);
    }
    else if (msg.msgType == EMessageType::PDU_TRANSMISSION_ACK)
    {
        auto &m = static_cast<const RlsPduTransmissionAck &>(msg);
        if (m.pduIds.size() > (MaxDatagramSize - 17) / 4)
            throw std::invalid_argument("Too many RLS acknowledgements");
        stream.appendOctet4(static_cast<uint32_t>(m.pduIds.size()));
        for (auto pduId : m.pduIds)
            stream.appendOctet4(pduId);
    }
    else if (msg.msgType == EMessageType::RELEASE_SESSION)
    {
        auto &m = static_cast<const RlsTerminateSession &>(msg);
        stream.appendOctet4(m.pduId);
        stream.appendOctet4(m.psi);
    }
    else if (msg.msgType == EMessageType::SESSION_TRANSMISSION)
    {
        auto &m = static_cast<const RlsSessionTransmission &>(msg);
        if (!m.m_pduSession || !m.m_pduSession->qosFlows)
            throw std::invalid_argument("Missing RLS session or QoS list");
        auto &session = *m.m_pduSession;
        auto &qosList = session.qosFlows->list;
        if (session.ueId < 0 || session.psi < 1 || session.psi > 15 ||
            static_cast<unsigned>(session.sessionType) > static_cast<unsigned>(PduSessionType::UNSTRUCTURED) ||
            qosList.count < 0 || qosList.count > static_cast<int>(MaxQosFlows) ||
            (qosList.count != 0 && !qosList.array))
            throw std::invalid_argument("Invalid RLS session fields");
        ValidateAddress(session.upTunnel.address);
        ValidateAddress(session.downTunnel.address);
        for (int i = 0; i < qosList.count; ++i)
        {
            if (!qosList.array[i] || qosList.array[i]->qosFlowIdentifier < 0 ||
                qosList.array[i]->qosFlowIdentifier >= MaxQosFlows)
                throw std::invalid_argument("Invalid RLS QoS flow identifier");
        }

        stream.appendOctet(SessionWireVersion);
        stream.appendOctet4(m.pduId);
        stream.appendOctet4(m.payload);
        stream.appendOctet8(m.amfId);
        stream.appendOctet4(session.ueId);
        stream.appendOctet4(session.psi);
        stream.appendOctet8(session.sessionAmbr.dlAmbr);
        stream.appendOctet8(session.sessionAmbr.ulAmbr);
        stream.appendOctet(static_cast<uint8_t>(session.dataForwardingNotPossible));
        stream.appendOctet(static_cast<uint8_t>(session.sessionType));
        stream.appendOctet4(session.upTunnel.teid);
        stream.appendOctet4(session.upTunnel.address.length());
        stream.append(session.upTunnel.address);
        stream.appendOctet4(session.downTunnel.teid);
        stream.appendOctet4(session.downTunnel.address.length());
        stream.append(session.downTunnel.address);
        stream.appendOctet4(qosList.count);
        for (int i = 0; i < qosList.count; ++i)
            stream.appendOctet4(static_cast<uint32_t>(qosList.array[i]->qosFlowIdentifier));

        // All peers built from this repository use this explicit presence/length.
        // A source gNB sends 0; a UE relaying the session to its target sends 4.
        stream.appendOctet(m.check ? 4 : 0);
        if (m.check)
            nas::IEUeSecurityCapability::Encode(m.m_ueSecurityCapability, stream);
    }
    else if (msg.msgType == EMessageType::XN_SESSION_TRANSMISSION)
    {
        auto &m = static_cast<const RlsXnSessionTransmission &>(msg);
        stream.appendOctet4(m.pduId);
        stream.appendOctet4(m.payload);
    }
    else
        throw std::invalid_argument("Unsupported RLS message type");
}
} // namespace

void EncodeRlsMessage(const RlsMessage &msg, OctetString &stream)
{
    OctetString encoded;
    encoded.appendOctet(0x03); // Original RLS compatibility marker.
    encoded.appendOctet(cons::Major);
    encoded.appendOctet(cons::Minor);
    encoded.appendOctet(cons::Patch);
    encoded.appendOctet(static_cast<uint8_t>(msg.msgType));
    encoded.appendOctet8(msg.sti);
    EncodePayload(msg, encoded);
    if (static_cast<size_t>(encoded.length()) > MaxDatagramSize)
        throw std::invalid_argument("RLS message exceeds UDP datagram size");
    stream.append(encoded);
}

std::unique_ptr<RlsMessage> DecodeRlsMessage(const OctetView &stream)
{
    try
    {
        Reader reader(stream);
        if (reader.remaining() > MaxDatagramSize)
            return nullptr;
        reader.require(13);
        if (reader.byte() != 3 || reader.byte() != cons::Major || reader.byte() != cons::Minor ||
            reader.byte() != cons::Patch)
            return nullptr;
        auto type = static_cast<EMessageType>(reader.byte());
        auto sti = reader.longWord();
        std::unique_ptr<RlsMessage> result;
        if (type == EMessageType::HEARTBEAT)
        {
            auto res = std::make_unique<RlsHeartBeat>(sti);
            res->simPos.x = static_cast<int32_t>(reader.word());
            res->simPos.y = static_cast<int32_t>(reader.word());
            res->simPos.z = static_cast<int32_t>(reader.word());
            result = std::move(res);
        }
        else if (type == EMessageType::HEARTBEAT_ACK)
        {
            auto res = std::make_unique<RlsHeartBeatAck>(sti);
            res->dbm = static_cast<int32_t>(reader.word());
            result = std::move(res);
        }
        else if (type == EMessageType::PDU_TRANSMISSION)
        {
            auto res = std::make_unique<RlsPduTransmission>(sti);
            res->pduType = static_cast<EPduType>(reader.byte());
            if (res->pduType != EPduType::RRC && res->pduType != EPduType::DATA)
                return nullptr;
            res->pduId = reader.word();
            res->payload = reader.word();
            res->pdu = reader.bytes(reader.word());
            result = std::move(res);
        }
        else if (type == EMessageType::PDU_TRANSMISSION_ACK)
        {
            auto res = std::make_unique<RlsPduTransmissionAck>(sti);
            auto count = reader.word();
            if (count > reader.remaining() / 4)
                return nullptr;
            res->pduIds.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
                res->pduIds.push_back(reader.word());
            result = std::move(res);
        }
        else if (type == EMessageType::RELEASE_SESSION)
        {
            auto res = std::make_unique<RlsTerminateSession>(sti);
            res->pduId = reader.word();
            res->psi = reader.word();
            result = std::move(res);
        }
        else if (type == EMessageType::SESSION_TRANSMISSION)
        {
            if (reader.byte() != SessionWireVersion)
                return nullptr;
            auto res = std::make_unique<RlsSessionTransmission>(sti);
            res->pduId = reader.word();
            res->payload = reader.word();
            res->amfId = reader.longWord();
            auto ueId = reader.word();
            auto psi = reader.word();
            if (ueId > static_cast<uint32_t>(std::numeric_limits<int>::max()) || psi < 1 || psi > 15)
                return nullptr;
            auto session = std::make_unique<nr::gnb::PduSessionResource>(static_cast<int>(ueId), static_cast<int>(psi));
            session->sessionAmbr.dlAmbr = reader.longWord();
            session->sessionAmbr.ulAmbr = reader.longWord();
            auto forwarding = reader.byte();
            auto sessionType = reader.byte();
            if (forwarding > 1 || sessionType > static_cast<uint8_t>(PduSessionType::UNSTRUCTURED))
                return nullptr;
            session->dataForwardingNotPossible = forwarding != 0;
            session->sessionType = static_cast<PduSessionType>(sessionType);
            session->upTunnel.teid = reader.word();
            session->upTunnel.address = reader.address();
            session->downTunnel.teid = reader.word();
            session->downTunnel.address = reader.address();
            auto count = reader.word();
            if (count > MaxQosFlows || count > reader.remaining() / 4)
                return nullptr;
            // Use the ASN allocator and descriptor-aware deleter. Mixing new[]
            // with ASN_STRUCT_FREE, or omitting the descriptor, corrupts/leaks memory.
            auto qos = asn::WrapUnique(asn::New<ASN_NGAP_QosFlowSetupRequestList>(),
                                       asn_DEF_ASN_NGAP_QosFlowSetupRequestList);
            if (!qos)
                throw std::bad_alloc{};
            for (uint32_t i = 0; i < count; ++i)
            {
                auto qfi = reader.word();
                if (qfi >= MaxQosFlows)
                    return nullptr;
                auto item = asn::WrapUnique(asn::New<ASN_NGAP_QosFlowSetupRequestItem>(),
                                            asn_DEF_ASN_NGAP_QosFlowSetupRequestItem);
                if (!item)
                    throw std::bad_alloc{};
                item->qosFlowIdentifier = qfi;
                if (ASN_SEQUENCE_ADD(&qos->list, item.get()) != 0)
                    throw std::bad_alloc{};
                item.release();
            }
            session->qosFlows = std::move(qos);
            auto capabilityLength = reader.byte();
            if (capabilityLength != 0 && capabilityLength != 4)
                return nullptr;
            reader.require(capabilityLength);
            res->check = capabilityLength != 0;
            if (res->check)
                res->m_ueSecurityCapability = nas::IEUeSecurityCapability::Decode(stream, capabilityLength);
            res->m_pduSession = std::move(session);
            result = std::move(res);
        }
        else if (type == EMessageType::XN_SESSION_TRANSMISSION)
        {
            auto res = std::make_unique<RlsXnSessionTransmission>(sti);
            res->pduId = reader.word();
            res->payload = reader.word();
            result = std::move(res);
        }
        else
            return nullptr;
        reader.end();
        return result;
    }
    catch (const InvalidMessage &)
    {
        return nullptr;
    }
}

} // namespace rls
