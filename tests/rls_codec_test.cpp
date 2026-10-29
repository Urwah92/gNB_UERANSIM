#include <lib/rls/rls_pdu.hpp>
#include <asn/ngap/ASN_NGAP_QosFlowSetupRequestItem.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void Require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::unique_ptr<rls::RlsMessage> Decode(const OctetString &bytes)
{
    return rls::DecodeRlsMessage(OctetView(bytes));
}

OctetString Encode(const rls::RlsMessage &message)
{
    OctetString bytes;
    rls::EncodeRlsMessage(message, bytes);
    return bytes;
}

void SetWord(OctetString &bytes, int offset, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        bytes.data()[offset + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
}

void CheckRoundTrip(const rls::RlsMessage &message)
{
    auto bytes = Encode(message);
    auto decoded = Decode(bytes);
    Require(decoded && decoded->msgType == message.msgType && decoded->sti == message.sti,
            "Message type/STI lost in round trip");
    auto again = Encode(*decoded);
    Require(bytes == again, "Re-encoded message differs");

    // Every possible truncation includes counts, lengths, fixed-width fields,
    // QoS items, and the optional capability. No partial datagram is accepted.
    for (int length = 0; length < bytes.length(); ++length)
        Require(!rls::DecodeRlsMessage(OctetView(bytes.data(), static_cast<size_t>(length))),
                "Truncated packet accepted");
    auto padded = bytes.copy();
    padded.appendOctet(0);
    Require(!Decode(padded), "Unexpected trailing bytes accepted");
}

rls::RlsSessionTransmission Session(const std::vector<uint32_t> &qfis, bool capability)
{
    rls::RlsSessionTransmission message{0x0123456789ABCDEFULL};
    message.pduId = 41;
    message.payload = 7;
    message.amfId = 0xFABCDE1234ULL; // AMF UE NGAP ID exceeds 32 bits.
    message.m_pduSession = std::make_unique<nr::gnb::PduSessionResource>(41, 7);
    auto &session = *message.m_pduSession;
    session.sessionAmbr.dlAmbr = 0x1122334455667788ULL;
    session.sessionAmbr.ulAmbr = 9000000000ULL;
    session.dataForwardingNotPossible = true;
    session.sessionType = PduSessionType::IPv4;
    session.upTunnel.teid = 0x11223344;
    session.upTunnel.address = OctetString::FromHex("c0000201");
    session.downTunnel.teid = 0x55667788;
    session.downTunnel.address = OctetString::FromHex("20010db8000000000000000000000001");
    session.qosFlows = asn::WrapUnique(asn::New<ASN_NGAP_QosFlowSetupRequestList>(),
                                      asn_DEF_ASN_NGAP_QosFlowSetupRequestList);
    Require(static_cast<bool>(session.qosFlows), "QoS allocation failed");
    for (auto qfi : qfis)
    {
        auto item = asn::WrapUnique(asn::New<ASN_NGAP_QosFlowSetupRequestItem>(),
                                    asn_DEF_ASN_NGAP_QosFlowSetupRequestItem);
        Require(static_cast<bool>(item), "QoS item allocation failed");
        item->qosFlowIdentifier = qfi;
        Require(ASN_SEQUENCE_ADD(&session.qosFlows->list, item.get()) == 0, "QoS append failed");
        item.release();
    }
    message.check = capability;
    // NAS uses signed one-bit fields: -1 is the representable set-bit value.
    message.m_ueSecurityCapability.b_128_5G_EA1 = -1;
    message.m_ueSecurityCapability.b_128_5G_IA2 = -1;
    message.m_ueSecurityCapability.b_128_EEA3 = -1;
    message.m_ueSecurityCapability.b_EIA7 = -1;
    return message;
}

void StandardMessages()
{
    rls::RlsHeartBeat heartbeat{0x0102030405060708ULL};
    heartbeat.simPos = {-10, 20, -30};
    CheckRoundTrip(heartbeat);
    auto bytes = Encode(heartbeat);
    Require(bytes.toHexString() == "03030206040102030405060708FFFFFFF600000014FFFFFFE2",
            "Original heartbeat wire format changed");

    rls::RlsHeartBeatAck heartbeatAck{73};
    heartbeatAck.dbm = -81;
    CheckRoundTrip(heartbeatAck);
    for (auto type : {rls::EPduType::RRC, rls::EPduType::DATA})
    {
        rls::RlsPduTransmission pdu{123};
        pdu.pduType = type;
        pdu.pduId = 0xFFFFFFFF;
        pdu.payload = 0x12345678;
        pdu.pdu = OctetString::FromHex("0001feff0102030405");
        CheckRoundTrip(pdu);
        auto malformed = Encode(pdu);
        SetWord(malformed, 22, 0xFFFFFFFF);
        Require(!Decode(malformed), "Oversized PDU length accepted");
        malformed.data()[13] = 255;
        Require(!Decode(malformed), "Unknown PDU type accepted");
        pdu.pdu = {};
        CheckRoundTrip(pdu);
    }
    rls::RlsPduTransmissionAck ack{123};
    CheckRoundTrip(ack);
    ack.pduIds = {0, 1, 0xFFFFFFFF};
    CheckRoundTrip(ack);
    auto malformed = Encode(ack);
    SetWord(malformed, 13, 0xFFFFFFFF);
    Require(!Decode(malformed), "Unbounded ACK count accepted");

    rls::RlsXnSessionTransmission xn{123};
    xn.pduId = 5;
    xn.payload = 7;
    CheckRoundTrip(xn);
    rls::RlsTerminateSession release{123};
    release.pduId = 5;
    release.psi = 7;
    CheckRoundTrip(release);
}

void SessionMessages()
{
    for (bool capability : {false, true})
    {
        for (auto qfis : {std::vector<uint32_t>{}, std::vector<uint32_t>{9}, std::vector<uint32_t>{0, 9, 63}})
        {
            auto message = Session(qfis, capability);
            CheckRoundTrip(message);
            auto bytes = Encode(message);
            auto decoded = Decode(bytes);
            auto &res = static_cast<rls::RlsSessionTransmission &>(*decoded);
            Require(res.check == capability, "Capability presence not preserved");
            Require(res.amfId == message.amfId, "64-bit AMF ID truncated");
            auto &list = res.m_pduSession->qosFlows->list;
            Require(list.count == static_cast<int>(qfis.size()) && list.size >= list.count,
                    "QoS count/allocation mismatch");
            Require(res.m_pduSession->qosFlows.get_deleter().desc == &asn_DEF_ASN_NGAP_QosFlowSetupRequestList,
                    "Decoded QoS list has no ASN deleter");
            for (int i = 0; i < list.count; ++i)
                Require(list.array[i] && list.array[i]->qosFlowIdentifier == qfis[i], "QoS flow lost");
            if (capability)
                Require(res.m_ueSecurityCapability.b_128_5G_EA1 && res.m_ueSecurityCapability.b_128_5G_IA2 &&
                            res.m_ueSecurityCapability.b_128_EEA3 && res.m_ueSecurityCapability.b_EIA7,
                        "Security capability bits lost");
        }
    }

    auto message = Session({1, 2, 3}, false);
    auto bytes = Encode(message);
    const int qosCountOffset = bytes.length() - 1 - 3 * 4 - 4;
    auto malformed = bytes.copy();
    SetWord(malformed, qosCountOffset, 0xFFFFFFFF);
    Require(!Decode(malformed), "Oversized QoS count accepted");
    malformed = bytes.copy();
    SetWord(malformed, qosCountOffset + 4, 64);
    Require(!Decode(malformed), "Out-of-range QFI accepted");
    malformed = bytes.copy();
    SetWord(malformed, 60, 0xFFFFFFFF);
    Require(!Decode(malformed), "Oversized tunnel address accepted");
    malformed = bytes.copy();
    malformed.data()[malformed.length() - 1] = 3;
    Require(!Decode(malformed), "Unknown capability length accepted");
    malformed = bytes.copy();
    malformed.data()[malformed.length() - 1] = 4;
    Require(!Decode(malformed), "Missing capability bytes accepted");
    malformed = bytes.copy();
    malformed.data()[13] = 0;
    Require(!Decode(malformed), "Legacy/unversioned session accepted");

    // Exercise freeing the complete decoded ASN list repeatedly under ASan/LSan.
    for (int i = 0; i < 1000; ++i)
        Require(static_cast<bool>(Decode(bytes)), "Repeated session decode failed");
}

void InvalidInputs()
{
    rls::RlsSessionTransmission missingSession{1};
    Require(!missingSession.check, "Capability presence defaults to an indeterminate value");
    OctetString output = OctetString::FromHex("aabb");
    try
    {
        rls::EncodeRlsMessage(missingSession, output);
        throw std::runtime_error("Missing session encoded");
    }
    catch (const std::invalid_argument &)
    {
    }
    Require(output.toHexString() == "AABB", "Failed encode modified caller buffer");
    auto message = Session({1}, false);
    auto bytes = Encode(message);
    bytes.data()[4] = 255;
    Require(!Decode(bytes), "Unknown message type accepted");
    std::vector<uint8_t> oversized(65508, 0);
    Require(!rls::DecodeRlsMessage(OctetView(oversized.data(), oversized.size())), "Oversized datagram accepted");
}
} // namespace

int main()
{
    try
    {
        StandardMessages();
        SessionMessages();
        InvalidInputs();
        std::cout << "RLS codec tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
