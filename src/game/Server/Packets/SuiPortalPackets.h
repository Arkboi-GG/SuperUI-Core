/*
 * SuperUI real-portal extension -- strict client packet layouts.
 *
 * These packets are private to MSUIClient and this core fork. All integers
 * are little-endian and game-object guids are raw uint64 values, never packed.
 */

#ifndef MANGOSSERVER_PACKETS_SUI_PORTAL_H
#define MANGOSSERVER_PACKETS_SUI_PORTAL_H

#include "Packet.h"
#include "ObjectGuid.h"

namespace WorldPackets
{
    namespace SuiPortal
    {
        static constexpr uint8 SUI_PORTAL_PROTOCOL_VERSION = 1;
        static constexpr size_t PREPARE_WIRE_SIZE = 16;
        static constexpr size_t READY_WIRE_SIZE = 28;

        class Prepare final : public ClientPacket
        {
        public:
            uint8 version = 0;
            uint8 reserved = 0;
            uint16 flags = 0;
            uint32 requestId = 0;
            ObjectGuid portalGuid;
            bool exactSize = false;

            explicit Prepare() : ClientPacket(CMSG_SUI_PORTAL_PREPARE) {}

            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                exactSize = recv_data.size() == PREPARE_WIRE_SIZE;
                if (!exactSize)
                {
                    recv_data.rfinish();
                    return;
                }

                recv_data >> version >> reserved >> flags >> requestId >> portalGuid;
            }
        };

        class Ready final : public ClientPacket
        {
        public:
            uint8 version = 0;
            uint8 result = 0;
            uint16 reserved = 0;
            ObjectGuid portalGuid;
            uint32 generation = 0;
            uint32 revision = 0;
            uint64 ticket = 0;
            bool exactSize = false;

            explicit Ready() : ClientPacket(CMSG_SUI_PORTAL_READY) {}

            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                exactSize = recv_data.size() == READY_WIRE_SIZE;
                if (!exactSize)
                {
                    recv_data.rfinish();
                    return;
                }

                recv_data >> version >> result >> reserved >> portalGuid;
                recv_data >> generation >> revision >> ticket;
            }
        };
    }
}

#endif
