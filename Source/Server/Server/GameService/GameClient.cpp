// Dark Souls 3 - Open Server

#include "Server/GameService/GameClient.h"
#include "Server/GameService/GameService.h"
#include "Server/Streams/Frpg2UdpPacketStream.h"
#include "Server/Streams/Frpg2UdpPacket.h"

#include "Server/Server.h"

#include "Platform/Platform.h"

#include "Core/Utils/Logging.h"
#include "Core/Network/NetConnection.h"

#include "Config/BuildConfig.h"
#include "Config/RuntimeConfig.h"

#include "Protobuf/Frpg2RequestMessage.pb.h"

GameClient::GameClient(GameService* OwningService, std::shared_ptr<NetConnection> InConnection, const std::vector<uint8_t>& CwcKey, uint64_t AuthToken)
    : Service(OwningService)
    , Connection(InConnection)
{
    LastMessageRecievedTime = GetSeconds();

    MessageStream = std::make_shared<Frpg2UdpPacketStream>(InConnection, CwcKey, AuthToken);
}

bool GameClient::Poll()
{
    // Has this client timed out?
    double TimeSinceLastMessage = GetSeconds() - LastMessageRecievedTime;
    if (TimeSinceLastMessage >= BuildConfig::CLIENT_TIMEOUT)
    {
        Warning("[%s] Client timed out.", GetName().c_str());
        return true;
    }

    // Client disconnected.
    if (Connection->Pump())
    {
        Warning("[%s] Disconnecting client as connection was in an error state.", GetName().c_str());
        return true;
    }
    if (!Connection->IsConnected())
    {
        Warning("[%s] Client disconnected.", GetName().c_str());
        return true;
    }

    // Pump the message stream and handle any messages that come in.
    if (MessageStream->Pump())
    {
        Warning("[%s] Disconnecting client as message stream was in an error state.", GetName().c_str());
        return true;
    }

    // Process all packets.
    Frpg2UdpPacket Packet;
    while (MessageStream->Recieve(&Packet))
    {
        // TODO: Refresh authentication state each time a packet is recieved.
        Service->RefreshAuthToken(Packet.Header.auth_token);
    }

    return false;
}

std::string GameClient::GetName()
{
    return Connection->GetName();
}