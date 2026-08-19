/* RTS-only same-faction AiBot discovery and direct-control authorization. */

#ifndef MANGOS_SUI_FACTION_CONTROL_H
#define MANGOS_SUI_FACTION_CONTROL_H

#include "Common.h"

class Player;
class WorldSession;

namespace SuiFactionControl
{
    enum RelocateResult : uint8
    {
        RELOCATE_NOT_ALLOWED = 0,
        RELOCATE_ACCEPTED = 1,
        RELOCATE_INSTANCE_DENIED = 2,
    };

    // This is the only party-membership bypass. It is false unless both the
    // immutable RTS boot latch and control.faction_bots are active.
    bool CanControl(Player const* actor, Player const* bot);
    RelocateResult TryRelocate(Player* actor, Player* bot);

    void HandleRoster(WorldSession* session, uint8 flags, uint32 requestId,
        uint32 zoneId, uint32 afterGuidLow, uint8 limit);
}

#endif
