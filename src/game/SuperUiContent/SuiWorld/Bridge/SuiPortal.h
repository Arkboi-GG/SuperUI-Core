/*
 * Server-authoritative preparation/readiness handshake for MSUIClient's
 * rendered mage portals. The stock CMSG_GAMEOBJ_USE path remains untouched.
 */

#ifndef MANGOS_SUI_PORTAL_H
#define MANGOS_SUI_PORTAL_H

#include "Common.h"

class GameObject;
class WorldPacket;

namespace SuiPortal
{
    // Optional trailer on SMSG_SUI_CONTROL_ACK. A current MSUIClient sends a
    // zero-guid request through that old, backwards-compatible opcode before it
    // ever emits a portal opcode; old cores simply deny it with no trailer.
    constexpr uint32 CAPABILITIES_MAGIC = 0x31495553; // "SUI1" little-endian
    constexpr uint32 CAPABILITY_REAL_PORTALS_V1 = 1u << 0;
    constexpr uint32 CAPABILITY_PORTAL_CAST_PREWARM_V1 = 1u << 1;
    // Faction bot control groups (census + non-party possession + RTS orders)
    // are served by this core. Baseline SuperUI — advertised unconditionally.
    constexpr uint32 CAPABILITY_FACTION_CONTROL_GROUPS_V1 = 1u << 2;

    // Append the backwards-compatible capability trailer and, when all six
    // server-authored destinations are available, the fixed-row cast-prewarm
    // catalog consumed before a summoned portal GameObject has a GUID.
    void WriteCapabilityTrailer(WorldPacket& data);

    // Summoned Mage portals need to reach an SUI client before the ordinary
    // continent visibility radius so its per-player 150-yard warm can begin.
    // This changes streaming visibility only; stock interaction range remains.
    void ConfigureSummonedPortalVisibility(GameObject* portal, uint32 summonSpellId);

    enum DescriptorResult : uint8
    {
        DESCRIPTOR_OK = 0,
        DESCRIPTOR_DENIED = 1,
        DESCRIPTOR_UNSUPPORTED = 2,
        DESCRIPTOR_EXPIRED = 3,
        DESCRIPTOR_FAILED = 4,
    };

    enum LoadResult : uint8
    {
        LOAD_READY = 0,
        LOAD_FAILED = 1,
    };

    enum PortalState : uint8
    {
        STATE_READY = 0,
        STATE_REVOKED = 1,
        STATE_BLOCKED = 2,
        STATE_ENTERING = 3,
        STATE_EXPIRED = 4,
        STATE_FAILED = 5,
    };

    enum StateReason : uint8
    {
        REASON_NONE = 0,
        REASON_BAD_PACKET = 1,
        REASON_UNSUPPORTED_VERSION = 2,
        REASON_CORRELATION = 3,
        REASON_LEASE_EXPIRED = 4,
        REASON_OBJECT_UNAVAILABLE = 5,
        REASON_NOT_AUTHORIZED = 6,
        REASON_CLIENT_LOAD_FAILED = 7,
    };
}

#endif
