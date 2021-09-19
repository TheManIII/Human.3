/*
 * Dark Souls 3 - Open Server
 * Copyright (C) 2021 Tim Leonard
 *
 * This program is free software; licensed under the MIT license.
 * You should have received a copy of the license along with this program.
 * If not, see <https://opensource.org/licenses/MIT>.
 */

#include "Server/Streams/Frpg2ReliableUdpPacketStream.h"
#include "Server/Streams/Frpg2ReliableUdpPacket.h"
#include "Server/Streams/Frpg2UdpPacket.h"

#include "Config/BuildConfig.h"

#include "Core/Network/NetConnection.h"

#include "Core/Utils/Logging.h"
#include "Core/Utils/File.h"
#include "Core/Utils/Strings.h"

#include "Core/Crypto/RSAKeyPair.h"
#include "Core/Crypto/RSACipher.h"

#include <thread>
#include <chrono>

Frpg2ReliableUdpPacketStream::Frpg2ReliableUdpPacketStream(std::shared_ptr<NetConnection> Connection, const std::vector<uint8_t>& CwcKey, uint64_t AuthToken, bool AsClient)
    : Frpg2UdpPacketStream(Connection, CwcKey, AuthToken, AsClient)
{
    Reset();
}

void Frpg2ReliableUdpPacketStream::Disconnect()
{
    if (State == Frpg2ReliableUdpStreamState::Established)
    {
        Send_FIN();
    }
}

void Frpg2ReliableUdpPacketStream::Connect(const std::string& ClientSteamId)
{
    State = Frpg2ReliableUdpStreamState::Connecting;
    SteamId = ClientSteamId;
    ResendSynTimer = GetSeconds();

    Send_SYN();
}

bool Frpg2ReliableUdpPacketStream::Send(const Frpg2ReliableUdpPacket& Input)
{
    // Swallow any packets being sent while we are closing.
    if (State == Frpg2ReliableUdpStreamState::Closing)
    {
        return true;
    }

    if (IsOpcodeSequenced(Input.Header.opcode) || Input.Header.opcode == Frpg2ReliableUdpOpCode::Unset)
    {
        Frpg2ReliableUdpPacket SentPacket = Input;
        SentPacket.SendTime = GetSeconds();

        // Opcode note set, we fill in the opcode and ack counters then
        // otherwise we assume the sender has dealt with it.
        if (SentPacket.Header.opcode == Frpg2ReliableUdpOpCode::Unset)
        {
            uint32_t Local, Remote;
            SentPacket.Header.GetAckCounters(Local, Remote);

            SentPacket.Header.SetAckCounters(SequenceIndex, Remote);

            if (Remote > 0)
            {
                SentPacket.Header.opcode = Frpg2ReliableUdpOpCode::DAT_ACK;
                DatAckResponses.insert(Remote);

                RemoteSequenceIndexAcked = Remote;
            }
            else
            {
                SentPacket.Header.opcode = Frpg2ReliableUdpOpCode::DAT;
            }
        }

        SequenceIndex = (SequenceIndex + 1) % MAX_ACK_VALUE;
        
        SendQueue.push_back(SentPacket);
    }
    else
    {
        SendRaw(Input);
    }

    return true;
}

bool Frpg2ReliableUdpPacketStream::Recieve(Frpg2ReliableUdpPacket* Output)
{
    if (RecieveQueue.size() > 0)
    {
        *Output = RecieveQueue[0];
        RecieveQueue.erase(RecieveQueue.begin());

        return true;
    }

    return false;
}

bool Frpg2ReliableUdpPacketStream::DecodeReliablePacket(const Frpg2UdpPacket& Input, Frpg2ReliableUdpPacket& Output)
{
    if (Input.Payload.size() < sizeof(Frpg2ReliableUdpPacketHeader))
    {
        Warning("[%s] Packet payload is less than the minimum size of a message, failed to deserialize.", Connection->GetName().c_str());
        InErrorState = true;
        return false;
    }

    int HeaderOffset = 0;
    int PayloadOffset = sizeof(Frpg2ReliableUdpPacketHeader);
    int PayloadSize = (int)Input.Payload.size() - sizeof(Frpg2ReliableUdpPacketHeader);    
    Ensure(Input.Payload[HeaderOffset] == 0xF5 && Input.Payload[HeaderOffset + 1] == 0x02);
    Output.Payload.resize(PayloadSize);

    memcpy(&Output.Header, Input.Payload.data() + HeaderOffset, sizeof(Frpg2ReliableUdpPacketHeader));
    memcpy(Output.Payload.data(), Input.Payload.data() + PayloadOffset, PayloadSize);

    //Output.Header.SwapEndian();

    return true;
}

bool Frpg2ReliableUdpPacketStream::EncodeReliablePacket(const Frpg2ReliableUdpPacket& Input, Frpg2UdpPacket& Output)
{
    Frpg2ReliableUdpPacket ByteSwappedMessage = Input;
    //ByteSwappedMessage.Header.SwapEndian();

    Output.HasConnectionPrefix = false;
    Output.Payload.resize(0);

    // Before the SYN we have to append the steam id data.
    if (Input.Header.opcode == Frpg2ReliableUdpOpCode::SYN)
    {
        Frpg2ReliableUdpInitialData InitialData;
        strcpy(InitialData.steam_id, SteamId.c_str());
        strcpy(InitialData.steam_id_copy, SteamId.c_str());

        Output.Payload.resize(sizeof(Frpg2ReliableUdpInitialData));
        memcpy(Output.Payload.data(), &InitialData, sizeof(Frpg2ReliableUdpInitialData));

        Output.HasConnectionPrefix = true;

        //Warning("[%s] Sending initial connection data.", Connection->GetName().c_str());
    }

    size_t Offset = Output.Payload.size();

    Output.Payload.resize(Offset + sizeof(Frpg2ReliableUdpPacketHeader) + Input.Payload.size());

    memcpy(Output.Payload.data() + Offset, &ByteSwappedMessage.Header, sizeof(Frpg2ReliableUdpPacketHeader));
    memcpy(Output.Payload.data() + Offset + sizeof(Frpg2ReliableUdpPacketHeader), Input.Payload.data(), Input.Payload.size());

    return true;
}

void Frpg2ReliableUdpPacketStream::HandleIncoming()
{
    // Accept any packets currently being recieved.
    while (true)
    {
        Frpg2UdpPacket Packet;
        if (!Frpg2UdpPacketStream::Recieve(&Packet))
        {
            break;
        }

        // This is the initial packet that contains the connection data before it.
        // Strip this data off, we don't really care about it, just some steam id's.
        if (Packet.Payload.size() > sizeof(Frpg2ReliableUdpInitialData) && Packet.Payload[0] != 0xF5 && Packet.Payload[0] != 0x25)
        {
            Frpg2ReliableUdpInitialData InitialData;
            memcpy(&InitialData, Packet.Payload.data(), sizeof(Frpg2ReliableUdpInitialData));

            std::vector<uint8_t> StrippedPayload;
            StrippedPayload.resize(Packet.Payload.size() - sizeof(Frpg2ReliableUdpInitialData));
            memcpy(StrippedPayload.data(), Packet.Payload.data() + sizeof(Frpg2ReliableUdpInitialData), StrippedPayload.size());
            Packet.Payload = StrippedPayload;

            //Warning("[%s] Initial connection data recieve, steam-id: %s", Connection->GetName().c_str(), InitialData.steam_id);
        }

        Frpg2ReliableUdpPacket ReliablePacket;
        if (!DecodeReliablePacket(Packet, ReliablePacket))
        {
            Warning("[%s] Failed to convert packet payload to message.", Connection->GetName().c_str());
            InErrorState = true;
            continue;
        }

        // Disassemble if required.
        if constexpr (BuildConfig::DISASSEMBLE_RECIEVED_MESSAGES)
        {
            ReliablePacket.Disassembly = Disassemble(ReliablePacket);

            if (ReliablePacket.Header.opcode != Frpg2ReliableUdpOpCode::DAT &&
                ReliablePacket.Header.opcode != Frpg2ReliableUdpOpCode::DAT_ACK)
            {
                Log("\n<< RECV\n%s", ReliablePacket.Disassembly.c_str());
            }
        }

        HandleIncomingPacket(ReliablePacket);
    }

    // Process as many packets as we can off the pending queue.
    while (PendingRecieveQueue.size() > 0)
    {
        Frpg2ReliableUdpPacket& Next = PendingRecieveQueue[0];
        
        uint32_t Local, Remote;
        Next.Header.GetAckCounters(Local, Remote);

        if (Local == GetNextRemoteSequenceIndex())
        {
            //Log("[%s] Processing next packet in sequence %i.", Connection->GetName().c_str(), RemoteSequenceIndex + 1);

            ProcessPacket(Next);
      
            PendingRecieveQueue.erase(PendingRecieveQueue.begin());

            RemoteSequenceIndex = (RemoteSequenceIndex + 1) % MAX_ACK_VALUE;
        }
        else
        {
            break;
        }
    }
}

int Frpg2ReliableUdpPacketStream::GetPacketIndexByLocalSequence(const std::vector<Frpg2ReliableUdpPacket>& Queue, uint32_t SequenceIndex)
{
    for (size_t i = 0; i < Queue.size(); i++)
    {
        uint32_t Local, Remote;
        Queue[i].Header.GetAckCounters(Local, Remote);

        if (Local == SequenceIndex)
        {
            return (int)i;
        }
    }
    return -1;
}

bool Frpg2ReliableUdpPacketStream::IsOpcodeSequenced(Frpg2ReliableUdpOpCode Opcode)
{
    // Determines if an opcode causes incrementing of the sequence value and 
    // needs to be queue and sent via the normal retransmission channel. 
    // Otherwise it can be sent raw at any time and the sequence does not matter.

    return Opcode == Frpg2ReliableUdpOpCode::DAT ||
           Opcode == Frpg2ReliableUdpOpCode::DAT_ACK ||
           Opcode == Frpg2ReliableUdpOpCode::FIN_ACK;
}

void Frpg2ReliableUdpPacketStream::HandleIncomingPacket(const Frpg2ReliableUdpPacket& Packet)
{
    LastPacketRecievedTime = GetSeconds();

    uint32_t LocalAck, RemoteAck;
    Packet.Header.GetAckCounters(LocalAck, RemoteAck);

    if constexpr (BuildConfig::EMIT_RELIABLE_UDP_PACKET_STREAM)
    {
        EmitDebugInfo(true, Packet);
    }

    // Check sequence index to prune duplicate / out of order for relevant packets.
    if (IsOpcodeSequenced(Packet.Header.opcode))
    {
        if (State != Frpg2ReliableUdpStreamState::Established)
        {
            // TODO: Handle situation where the handshake is completed but we recieve a following
            // packet out of order.
            Warning("[%s] Recieved sequenced packets before connection is established, this is not allowed. ", Connection->GetName().c_str());
            InErrorState = true;
            return;
        }

        bool IsInCorrectSequence = false;

        // TODO: Fix this so we can queue up out of order packets and handle them when recieved. We had this before but causes issues
        //       when ack values overflow.
        if (LocalAck != GetNextRemoteSequenceIndex())
        {
            Warning("[%s] Ignoring incoming packet, out of sequence (incoming=%i head=%i). ", Connection->GetName().c_str(), LocalAck, RemoteSequenceIndex);
            IsInCorrectSequence = true;
        }

        if (GetPacketIndexByLocalSequence(PendingRecieveQueue, LocalAck) >= 0)
        {
            Warning("[%s] Ignoring incoming packet, duplicate that we already have. ", Connection->GetName().c_str());
            IsInCorrectSequence = true;
        }

        if (IsInCorrectSequence && (GetSeconds() - LastAckSendTime) > MIN_TIME_BETWEEN_RESEND_ACK)
        {   
            // Send an ACK, its possible that the remote is retransmitting packets as
            // a previously sent ACK has dropped.
            Log("Sending ack as not sent in a while.");

            Send_ACK(RemoteSequenceIndexAcked);

            return;
        }
        else if (!IsInCorrectSequence)
        {
            PendingRecieveQueue.push_back(Packet);
        }
    }
    else
    {
        ProcessPacket(Packet);
    }
}

void Frpg2ReliableUdpPacketStream::ProcessPacket(const Frpg2ReliableUdpPacket & Packet)
{
    uint32_t LocalAck, RemoteAck;
    Packet.Header.GetAckCounters(LocalAck, RemoteAck);

    //Log("[%s] Processing Packet: LocalAck=%i RemoteAck=%i", Connection->GetName().c_str(), LocalAck, RemoteAck);

    switch (Packet.Header.opcode)
    {
    case Frpg2ReliableUdpOpCode::SYN:
        {
            Handle_SYN(Packet);
            break;
        }
    case Frpg2ReliableUdpOpCode::SYN_ACK:
        {
            Handle_SYN_ACK(Packet);
            break;
        }
    case Frpg2ReliableUdpOpCode::DAT:
        {
            Handle_DAT(Packet);
            break;
        }
    case Frpg2ReliableUdpOpCode::HBT:
        {
            Handle_HBT(Packet);
            break;
        }
    case Frpg2ReliableUdpOpCode::FIN:
        {
            Handle_FIN(Packet);
            break;
        }
    case Frpg2ReliableUdpOpCode::RST:
        {
            Handle_RST(Packet);
            break;
        }
    case Frpg2ReliableUdpOpCode::ACK:
        {
            Handle_ACK(Packet);
            break;
        }
    case Frpg2ReliableUdpOpCode::RACK:
        {
            Handle_RACK(Packet);
            break;
        }
    case Frpg2ReliableUdpOpCode::DAT_ACK:
        {
            Handle_DAT_ACK(Packet);
            break;
        }
    case Frpg2ReliableUdpOpCode::FIN_ACK:
        {
            Handle_FIN_ACK(Packet);
            break;
        }
    default:
        {
            Error("[%s] Recieved unknown reliable udp opcode 0x%2x.", Connection->GetName().c_str(), Packet.Header.opcode);
            Ensure(false);
            break;
        }
    }
}

void Frpg2ReliableUdpPacketStream::Handle_SYN(const Frpg2ReliableUdpPacket& Packet)
{
    //Log("[%s] Recieved SYN, establishing handshake.", Connection->GetName().c_str());

    State = Frpg2ReliableUdpStreamState::SynRecieved;

    uint32_t InLocalAck, InRemoteAck;
    Packet.Header.GetAckCounters(InLocalAck, InRemoteAck);

    // Send a SYN_ACK in response.
    Send_SYN_ACK(InLocalAck);

    // And send our ACK message as well (this seems redundent, but its what happens in ds3).
    Send_ACK(InLocalAck);
}

void Frpg2ReliableUdpPacketStream::Handle_SYN_ACK(const Frpg2ReliableUdpPacket& Packet)
{
    //Log("[%s] Recieved SYN, establishing handshake.", Connection->GetName().c_str());

    State = Frpg2ReliableUdpStreamState::SynRecieved;

    uint32_t InLocalAck, InRemoteAck;
    Packet.Header.GetAckCounters(InLocalAck, InRemoteAck);

    RemoteSequenceIndex = InLocalAck;

    // And send our ACK message as well (this seems redundent, but its what happens in ds3).
    Send_ACK(RemoteSequenceIndex);

    // SYN_ACK bumps the sequence index so is a "sequenced opcode", but doesn't abid by
    // any of the other conventions of sequenced ones. So simplest to just bump the sequence
    // index here.
    SequenceIndex = (SequenceIndex + 1) % MAX_ACK_VALUE;
}

void Frpg2ReliableUdpPacketStream::Handle_HBT(const Frpg2ReliableUdpPacket& Packet)
{
    //Log("[%s] Recieved HBT.", Connection->GetName().c_str());

    uint32_t InLocalAck, InRemoteAck;
    Packet.Header.GetAckCounters(InLocalAck, InRemoteAck);

    // TODO: Handle overflow - This is super crude,  do it in a better way.
    if (SequenceIndexAcked > MAX_ACK_VALUE_TOP_QUART && InRemoteAck < MAX_ACK_VALUE_BOTTOM_QUART)
    {
        SequenceIndexAcked = InRemoteAck;
    }
    else
    {
        SequenceIndexAcked = std::max(SequenceIndexAcked, InRemoteAck);
    }

    Send_HBT();
}

void Frpg2ReliableUdpPacketStream::Handle_FIN(const Frpg2ReliableUdpPacket& Packet)
{
    //Log("[%s] Recieved FIN.", Connection->GetName().c_str());

    uint32_t InLocalAck, InRemoteAck;
    Packet.Header.GetAckCounters(InLocalAck, InRemoteAck);

    // TODO: We probably need to send a FIN_ACK here as well.
    Send_FIN_ACK(InLocalAck);

    State = Frpg2ReliableUdpStreamState::Closing;
}

void Frpg2ReliableUdpPacketStream::Handle_FIN_ACK(const Frpg2ReliableUdpPacket& Packet)
{
    // Don't set straight to closed, we want to wait till queues are drained first.
    State = Frpg2ReliableUdpStreamState::Closing;
}

void Frpg2ReliableUdpPacketStream::Handle_RST(const Frpg2ReliableUdpPacket& Packet)
{
    //Log("[%s] Recieved RST.", Connection->GetName().c_str());

    State = Frpg2ReliableUdpStreamState::Listening;
    Reset();
}

void Frpg2ReliableUdpPacketStream::Handle_ACK(const Frpg2ReliableUdpPacket& Packet)
{
    if (State == Frpg2ReliableUdpStreamState::SynRecieved)
    {
        //Log("[%s] Recieved initial ACK, handshake finished connection established.", Connection->GetName().c_str());
        State = Frpg2ReliableUdpStreamState::Established;
    }
    else
    {
        //Log("[%s] Recieved ACK.", Connection->GetName().c_str());
    }

    uint32_t InLocalAck, InRemoteAck;
    Packet.Header.GetAckCounters(InLocalAck, InRemoteAck);

    uint32_t SequenceIndexAckedOriginal = SequenceIndexAcked;
     
    // TODO: Handle overflow - This is super crude,  do it in a better way.
    if (SequenceIndexAcked > MAX_ACK_VALUE_TOP_QUART && InRemoteAck < MAX_ACK_VALUE_BOTTOM_QUART)
    {
        SequenceIndexAcked = InRemoteAck;
    }
    else
    {
        SequenceIndexAcked = std::max(SequenceIndexAcked, InRemoteAck);
    }
}
void Frpg2ReliableUdpPacketStream::Handle_RACK(const Frpg2ReliableUdpPacket& Packet)
{
    // I'm like 95% sure that RACK is "Reject ACK", its telling us the ACK recieved was invalid I think?
    // I think we can just ignore this ...

    Log("[%s] Recieved RACK - Ignoring ...", Connection->GetName().c_str());
}

void Frpg2ReliableUdpPacketStream::Handle_DAT(const Frpg2ReliableUdpPacket& Packet)
{
    //Log("[%s] Recieved DAT.", Connection->GetName().c_str());

    uint32_t InLocalAck, InRemoteAck;
    Packet.Header.GetAckCounters(InLocalAck, InRemoteAck);

    ExpectedDatAckResponses.insert(InLocalAck);

    RecieveQueue.push_back(Packet);

    Send_ACK(InLocalAck);
}

void Frpg2ReliableUdpPacketStream::Handle_DAT_ACK(const Frpg2ReliableUdpPacket& Packet)
{
    //Log("[%s] Recieved DAT_ACK.", Connection->GetName().c_str());

    uint32_t InLocalAck, InRemoteAck;
    Packet.Header.GetAckCounters(InLocalAck, InRemoteAck);

    uint32_t SequenceIndexAckedOriginal = SequenceIndexAcked;

    // TODO: Handle overflow - This is super crude,  do it in a better way.
    if (SequenceIndexAcked > MAX_ACK_VALUE_TOP_QUART && InRemoteAck < MAX_ACK_VALUE_BOTTOM_QUART)
    {
        SequenceIndexAcked = InRemoteAck;
    }
    else
    {
        SequenceIndexAcked = std::max(SequenceIndexAcked, InRemoteAck);
    }
    
    // Send an ACK for this DAT_ACK.
    Send_ACK(InLocalAck);

    RecieveQueue.push_back(Packet);
}

void Frpg2ReliableUdpPacketStream::Send_SYN()
{
    Frpg2ReliableUdpPacket SynRequest;
    SynRequest.Header.SetAckCounters(SequenceIndex, 0);
    SynRequest.Header.opcode = Frpg2ReliableUdpOpCode::SYN;

    Frpg2ReliableUdpPacketOpCodePayload_SYN SynPayload;

    SynRequest.Payload.resize(sizeof(SynPayload));
    memcpy(SynRequest.Payload.data(), &SynPayload, sizeof(SynPayload));

    Send(SynRequest);
}

void Frpg2ReliableUdpPacketStream::Send_SYN_ACK(uint32_t RemoteIndex)
{
    Frpg2ReliableUdpPacket SynAckResponse;
    SynAckResponse.Header.SetAckCounters(SequenceIndex, RemoteIndex);
    SynAckResponse.Header.opcode = Frpg2ReliableUdpOpCode::SYN_ACK;

    // TODO: Figure out these values, they seem to always be the same, but we 
    // should figure out what they are regardless.
    Frpg2ReliableUdpPacketOpCodePayload_SYN_ACK SynPayload;

    SynAckResponse.Payload.resize(sizeof(SynPayload));
    memcpy(SynAckResponse.Payload.data(), &SynPayload, sizeof(SynPayload));

    Send(SynAckResponse);

    RemoteSequenceIndex = RemoteIndex;

    // SYN_ACK bumps the sequence index so is a "sequenced opcode", but doesn't abid by
    // any of the other conventions of sequenced ones. So simplest to just bump the sequence
    // index here.
    SequenceIndex = (SequenceIndex + 1) % MAX_ACK_VALUE;
}

void Frpg2ReliableUdpPacketStream::Send_ACK(uint32_t RemoteIndex)
{
    Frpg2ReliableUdpPacket AckResponse;
    AckResponse.Header.SetAckCounters(0, RemoteIndex);
    AckResponse.Header.opcode = Frpg2ReliableUdpOpCode::ACK;

    Send(AckResponse);

    RemoteSequenceIndexAcked = RemoteIndex;
    LastAckSendTime = GetSeconds();
}

void Frpg2ReliableUdpPacketStream::Send_DAT_ACK(uint32_t LocalIndex, uint32_t RemoteIndex)
{
    Frpg2ReliableUdpPacket AckResponse;
    AckResponse.Header.SetAckCounters(LocalIndex, RemoteIndex);
    AckResponse.Header.opcode = Frpg2ReliableUdpOpCode::DAT_ACK;

    Send(AckResponse);

    RemoteSequenceIndexAcked = RemoteIndex;
    LastAckSendTime = GetSeconds();
}

void Frpg2ReliableUdpPacketStream::Send_FIN_ACK(uint32_t RemoteIndex)
{
    Frpg2ReliableUdpPacket AckResponse;
    AckResponse.Header.SetAckCounters(SequenceIndex, RemoteIndex);
    AckResponse.Header.opcode = Frpg2ReliableUdpOpCode::FIN_ACK;

    Send(AckResponse);
}

void Frpg2ReliableUdpPacketStream::Send_FIN()
{
    Frpg2ReliableUdpPacket AckResponse;
    AckResponse.Header.SetAckCounters(SequenceIndex, 0);
    AckResponse.Header.opcode = Frpg2ReliableUdpOpCode::FIN;

    Send(AckResponse);

    State = Frpg2ReliableUdpStreamState::Closing;
    CloseTimer = GetSeconds();
}

void Frpg2ReliableUdpPacketStream::Send_HBT()
{
    Frpg2ReliableUdpPacket HbtResponse;
    HbtResponse.Header.SetAckCounters(0, RemoteSequenceIndexAcked);
    HbtResponse.Header.opcode = Frpg2ReliableUdpOpCode::HBT;

    Send(HbtResponse);
}

bool Frpg2ReliableUdpPacketStream::SendRaw(const Frpg2ReliableUdpPacket& Input)
{
    uint32_t LocalAck, RemoteAck;
    Input.Header.GetAckCounters(LocalAck, RemoteAck);

    Ensure(Input.Header.opcode != Frpg2ReliableUdpOpCode::Unset);

    if constexpr (BuildConfig::EMIT_RELIABLE_UDP_PACKET_STREAM)
    {
        EmitDebugInfo(false, Input);
    }

    Frpg2UdpPacket Packet;
    if (!EncodeReliablePacket(Input, Packet))
    {
        Warning("[%s] Failed to convert message to packet payload.", Connection->GetName().c_str());
        InErrorState = true;
        return false;
    }

    // Disassemble if required.
    if constexpr (BuildConfig::DISASSEMBLE_SENT_MESSAGES)
    {
        Packet.Disassembly = Input.Disassembly;
        Packet.Disassembly.append(Disassemble(Input));

        Log("\n>> SENT\n%s", Packet.Disassembly.c_str());
    }

    if (!Frpg2UdpPacketStream::Send(Packet))
    {
        Warning("[%s] Failed to send.", Connection->GetName().c_str());
        InErrorState = true;
        return false;
    }

    return true;
}

void Frpg2ReliableUdpPacketStream::Reset()
{
    SequenceIndex = START_SEQUENCE_INDEX;
    SequenceIndexAcked = 0;
    RemoteSequenceIndex = 0;
    RemoteSequenceIndexAcked = 0;

    PendingRecieveQueue.clear();
    RecieveQueue.clear();    
    SendQueue.clear();
    RetransmitBuffer.clear();
}

void Frpg2ReliableUdpPacketStream::HandleOutgoing()
{
    // Trim off any retransmit packets that are not long relevant.
    for (auto iter = RetransmitBuffer.begin(); iter != RetransmitBuffer.end(); /* empty */)
    {
        Frpg2ReliableUdpPacket& Packet = *iter;

        uint32_t InLocalAck, InRemoteAck;
        Packet.Header.GetAckCounters(InLocalAck, InRemoteAck);

        // TODO: Handle overflow - This is super crude,  do it in a better way.
        if (InLocalAck > MAX_ACK_VALUE_TOP_QUART && SequenceIndexAcked < MAX_ACK_VALUE_BOTTOM_QUART ||
            InLocalAck <= SequenceIndexAcked)
        {
            //Log("[%s] Removing packet for retransmit buffer as its been acknowledged.", Connection->GetName().c_str());

            iter = RetransmitBuffer.erase(iter);
        }
        else
        {
            iter++;
        }
    }

    // If we have not had ack of packets in the retransmit queue for long enough, retransmit 
    // the first one and hope it gets acked soon.
    double CurrentTime = GetSeconds();
    if (!IsRetransmitting)
    {
        for (auto iter = RetransmitBuffer.begin(); iter != RetransmitBuffer.end(); iter++)
        {
            Frpg2ReliableUdpPacket& Packet = *iter;

            uint32_t InLocalAck, InRemoteAck;
            Packet.Header.GetAckCounters(InLocalAck, InRemoteAck);

            double ElapsedTime = (CurrentTime - Packet.SendTime);
            if (ElapsedTime > RETRANSMIT_INTERVAL)
            {
                Log("[%s] Starting retransmit as we have unacknowledged packets (packet %i).", Connection->GetName().c_str(), InLocalAck);

                SendRaw(Packet);

                IsRetransmitting = true;
                RetransmittingIndex = InLocalAck;
                RetransmitPacket = Packet;
                RetransmissionTimer = GetSeconds();
            }
        } 
    }
    // TODO: Handle overflow - This is super crude,  do it in a better way.
    else
    {
        double ElapsedTime = (CurrentTime - RetransmissionTimer);

        if (RetransmittingIndex > MAX_ACK_VALUE_TOP_QUART && SequenceIndexAcked < MAX_ACK_VALUE_BOTTOM_QUART ||
            SequenceIndexAcked >= RetransmittingIndex)
        {
            Log("[%s] Recovered from retransmit.", Connection->GetName().c_str());
            IsRetransmitting = false;
        }
        else if (ElapsedTime > RETRANSMIT_CYCLE_INTERVAL)
        {
            Log("[%s] Resending retransmission packet.", Connection->GetName().c_str());
            RetransmissionTimer = GetSeconds();
            SendRaw(RetransmitPacket);
        }
    }

    // Do not send any packets if we have a lot of packets waiting for ack.
    while (!IsRetransmitting && SendQueue.size() > 0 && RetransmitBuffer.size() < MAX_PACKETS_IN_FLIGHT)
    {
        Frpg2ReliableUdpPacket Packet = SendQueue[0];
        SendQueue.erase(SendQueue.begin());
        RetransmitBuffer.push_back(Packet);

        uint32_t InLocalAck, InRemoteAck;
        Packet.Header.GetAckCounters(InLocalAck, InRemoteAck);

        //Log("[%s] Sending sequenced packet, %i.", Connection->GetName().c_str(), InLocalAck);

        SendRaw(Packet);
    }
}

bool Frpg2ReliableUdpPacketStream::Pump()
{
    // Mark as connection closed after we have sent everything in the queue.
    if (State == Frpg2ReliableUdpStreamState::Closing && SendQueue.size() == 0)
    {
        Log("[%s] Connection closed.", Connection->GetName().c_str());
        State = Frpg2ReliableUdpStreamState::Closed;
    }

    // If connection is now closed, just drop all the packets.
    if (State == Frpg2ReliableUdpStreamState::Closed)
    {
        Reset();
        return true;
    }

    if (Frpg2UdpPacketStream::Pump())
    {
        return true;
    }

    // If connecting periodically resend the syn until we get a response. This helps punch a
    // hole through NAT if required.
    if (State == Frpg2ReliableUdpStreamState::Connecting)
    {
        double ResendElapsed = GetSeconds() - ResendSynTimer;
        if (ResendElapsed > RESEND_SYN_INTERVAL)
        {
            Send_SYN();
            ResendSynTimer = GetSeconds();
        }
    }

    // If closing and its taken too long then don't bother trying to gracefully disconnect.
    if (CloseTimer > 0.0f && State == Frpg2ReliableUdpStreamState::Closing)
    {        
        if (double Elapsed = GetSeconds() - CloseTimer; Elapsed > CONNECTION_CLOSE_TIMEOUT)
        {
            Log("[%s] Connection closing took to long, assuming connection terminated.", Connection->GetName().c_str());
            State = Frpg2ReliableUdpStreamState::Closed;        
            return true;
        }
    }

    HandleIncoming();
    HandleOutgoing();

    return false;
}

void Frpg2ReliableUdpPacketStream::EmitDebugInfo(bool Incoming, const Frpg2ReliableUdpPacket& Packet)
{
    uint32_t Local, Remote;
    Packet.Header.GetAckCounters(Local, Remote);
    Log("%s %-9s %-6i %-6i", Incoming ? "<<" : ">>", ToString(Packet.Header.opcode).c_str(), Local, Remote);
}

void Frpg2ReliableUdpPacketStream::HandledPacket(uint32_t AckSequence)
{
    if (auto iter = DatAckResponses.find(AckSequence); iter != DatAckResponses.end())
    {
        DatAckResponses.erase(iter);
        return;
    }
    
    bool NeedsDatAck = false;
    if (auto iter = ExpectedDatAckResponses.find(AckSequence); iter != ExpectedDatAckResponses.end())
    {
        ExpectedDatAckResponses.erase(iter);
        NeedsDatAck = true;
    }

    if (false)//NeedsDatAck)
    {
        Send_DAT_ACK(SequenceIndex, AckSequence);
    }
    else
    {
        Send_ACK(AckSequence);
    }
}

std::string Frpg2ReliableUdpPacketStream::Disassemble(const Frpg2ReliableUdpPacket& Message)
{
    std::string Result = "";

    uint32_t LocalAck, RemoteAck;
    Message.Header.GetAckCounters(LocalAck, RemoteAck);

    Result += "Reliable-Packet:\n";
    Result += StringFormat("\t%-30s = %u\n", "header_size", Message.Header.magic_number);
    Result += StringFormat("\t%-30s = %u\n", "local_ack", LocalAck);
    Result += StringFormat("\t%-30s = %u\n", "remote_ack", RemoteAck);
    Result += StringFormat("\t%-30s = %u\n", "opcode", Message.Header.opcode);
    Result += StringFormat("\t%-30s = %u\n", "unknown_1", Message.Header.unknown_1);

    if (Message.Header.opcode != Frpg2ReliableUdpOpCode::DAT &&
        Message.Header.opcode != Frpg2ReliableUdpOpCode::DAT_ACK)
    {
        Result += "Packet Payload:\n";
        Result += BytesToString(Message.Payload, "\t");
    }
    return Result;
}