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

            explicit Cam() : ClientPacket(CMSG_SUI_CAM) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                recv_data >> x >> y >> z;
            }
        };
    }
}

#endif
