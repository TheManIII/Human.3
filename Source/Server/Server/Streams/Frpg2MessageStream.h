// Dark Souls 3 - Open Server

#pragma once

#include "Server/Streams/Frpg2PacketStream.h"

#include "Protobuf/Frpg2RequestMessage.pb.h"

struct Frpg2Message;
class RSAKeyPair;
class Cipher;

class Frpg2MessageStream
    : public Frpg2PacketStream
{
public:
    Frpg2MessageStream(std::shared_ptr<NetConnection> Connection, RSAKeyPair* InEncryptionKey);

    // Returns true if send was successful, if false is returned the send queue
    // is likely saturated or the packet is invalid.
    virtual bool Send(const Frpg2Message& Message, uint32_t ResponseToRequestIndex = 0);

    // Short hand version of Send for protobufs, takes care of constructing the wrapper message.
    virtual bool Send(google::protobuf::MessageLite* Message, uint32_t ResponseToRequestIndex = 0);

    // Returns true if a packet was recieved and stores packet in OutputPacket.
    virtual bool Recieve(Frpg2Message* Message);

    // Changes the cipher used for encryption/descryption.
    virtual void SetCipher(std::shared_ptr<Cipher> Encryption, std::shared_ptr<Cipher> Decryption);

    std::shared_ptr<Cipher> GetEncryptionCipher() { return EncryptionCipher; }
    std::shared_ptr<Cipher> GetDecryptionCipher() { return DecryptionCipher; }

protected:

    bool PacketToMessage(const Frpg2Packet& Packet, Frpg2Message& Message);
    bool MessageToPacket(const Frpg2Message& Message, Frpg2Packet& Packet);

private:
    
    RSAKeyPair* EncryptionKey;

    std::shared_ptr<Cipher> EncryptionCipher;
    std::shared_ptr<Cipher> DecryptionCipher;
};