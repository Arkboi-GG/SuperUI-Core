/*
 * SuperUI CRPG/RTS control extension — client packets.
 *
 * These opcodes exist only between MSUIClient and this fork (see
 * docs/SUI_WIRE_PROTOCOL.md). Payloads are little-endian and fixed-layout;
 * guids travel as raw uint64 (never packed).
 */

#ifndef MANGOSSERVER_PACKETS_SUI_CONTROL_H
#define MANGOSSERVER_PACKETS_SUI_CONTROL_H

#include "Packet.h"
#include "ObjectGuid.h"

#include <vector>

namespace WorldPackets
{
    namespace SuiControl
    {
        class ControlRequest final : public ClientPacket
        {
        public:
            ObjectGuid targetGuid;

            explicit ControlRequest() : ClientPacket(CMSG_SUI_CONTROL_REQUEST) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                recv_data >> targetGuid;
            }
        };

        class ControlRelease final : public ClientPacket
        {
        public:
            uint8 mode = 0;             // SuiPossess::ReleaseMode

            explicit ControlRelease() : ClientPacket(CMSG_SUI_CONTROL_RELEASE) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                recv_data >> mode;
            }
        };

        class Order final : public ClientPacket
        {
        public:
            uint8 orderType = 0;        // SuiPossess::OrderType
            std::vector<ObjectGuid> subjects;   // empty = every controllable bot in the group
            ObjectGuid targetGuid;      // attack orders
            float x = 0, y = 0, z = 0;  // move orders

            explicit Order() : ClientPacket(CMSG_SUI_ORDER) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                recv_data >> orderType;
                uint8 count = 0;
                recv_data >> count;
                subjects.resize(count);
                for (uint8 i = 0; i < count; ++i)
                    recv_data >> subjects[i];
                recv_data >> targetGuid;
                recv_data >> x >> y >> z;
            }
        };

        class Cam final : public ClientPacket
        {
        public:
            float x = 0, y = 0, z = 0;  // free-camera position (raw WoW map coords)
            // Is the free view UP? The client cannot leave it silently: the server keys the
            // streaming eye and the commanded-remotely waiver off this. Optional on the wire so
            // a sender predating the flag still reads as "up", which is what it meant.
            uint8 active = 1;

            explicit Cam() : ClientPacket(CMSG_SUI_CAM) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                recv_data >> x >> y >> z;
                if (recv_data.rpos() < recv_data.size())
                    recv_data >> active;
            }
        };

        class ZoneIntel final : public ClientPacket
        {
        public:
            uint8 flags = 0;   // reserved; an empty body is legal (older sender)

            explicit ZoneIntel() : ClientPacket(CMSG_SUI_ZONE_INTEL) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                if (recv_data.rpos() < recv_data.size())
                    recv_data >> flags;
            }
        };

        class ForceRoster final : public ClientPacket
        {
        public:
            static constexpr size_t WIRE_SIZE = 14;

            uint8 flags = 0;
            uint32 requestId = 0;
            uint32 zoneId = 0;
            uint32 afterGuidLow = 0;
            uint8 limit = 0;
            bool exactSize = false;

            explicit ForceRoster() : ClientPacket(CMSG_SUI_FORCE_ROSTER) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                exactSize = recv_data.size() == WIRE_SIZE;
                if (!exactSize)
                {
                    recv_data.rfinish();
                    return;
                }

                recv_data >> flags >> requestId >> zoneId >> afterGuidLow >> limit;
            }
        };
    }
}

#endif
