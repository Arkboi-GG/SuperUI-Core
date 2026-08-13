/*
 * SuperUI RTS worldstate packets (tier-2). Conventions follow SuiControl.h:
 * little-endian, raw uint64 guids, optional-trailing reads for forward compat.
 */

#ifndef MANGOS_SUI_RTS_PACKETS_H
#define MANGOS_SUI_RTS_PACKETS_H

#include "Packet.h"

namespace WorldPackets
{
    namespace SuiRts
    {
        class RtsState final : public ClientPacket
        {
        public:
            uint8 flags = 0;   // reserved; an empty body is legal (older sender)

            explicit RtsState() : ClientPacket(CMSG_SUI_RTS_STATE) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                if (recv_data.rpos() < recv_data.size())
                    recv_data >> flags;
            }
        };

        class RtsAction final : public ClientPacket
        {
        public:
            uint8 action = 0;       // 1 heroDeclare / 2 heroUpgrade / 3 heroRevive
            uint64 subjectGuid = 0;

            explicit RtsAction() : ClientPacket(CMSG_SUI_RTS_ACTION) {}
            void ReadFromWorldPacket(WorldPacket& recv_data) override
            {
                recv_data >> action;
                recv_data >> subjectGuid;
            }
        };
    }
}

#endif
