/*
 * SuperUI tactical freeze (Command View).
 *
 * This is a localized, server-authoritative actor lock.  The map and its
 * sessions keep running; only Units latched by an active 100-yard field stop
 * advancing gameplay state.  See docs/SUI_WIRE_PROTOCOL.md for the v1 wire.
 */

#ifndef MANGOS_SUI_TACTICAL_FREEZE_H
#define MANGOS_SUI_TACTICAL_FREEZE_H

#include "Common.h"
#include "ObjectGuid.h"

#include <vector>

class Map;
class Player;
class Unit;
class WorldSession;

namespace SuiTacticalFreeze
{
    constexpr uint8 WIRE_VERSION = 1;
    constexpr float RADIUS_YARDS = 100.0f;
    // The wire carries a u16 count.  Do not impose a smaller gameplay cap:
    // every loaded Unit in the field must be represented or the lock fails
    // closed instead of leaving a partially-frozen radius.
    constexpr uint32 MAX_MEMBERS = 0xFFFFu;
    constexpr uint8 MAX_REQUEST_RECORDS = 40;
    constexpr uint8 MAX_ACTIONS_PER_ACTOR = 5;

    enum FreezeResult : uint8
    {
        FREEZE_OK                  = 0,
        FREEZE_DENIED_SESSION      = 1,
        FREEZE_DENIED_COMMAND_VIEW = 2,
        FREEZE_DENIED_STATE        = 3,
        FREEZE_ALREADY_ACTIVE      = 4,
        FREEZE_FROZEN_BY_OTHER     = 5,
        FREEZE_NOT_OWNER           = 6,
        FREEZE_NOT_FOUND           = 7,
        FREEZE_BAD_PACKET          = 8,
        FREEZE_RELEASED_VIEW       = 9,
        FREEZE_RELEASED_LOGOUT     = 10,
        FREEZE_RELEASED_MAP_CHANGE = 11,
        FREEZE_RELEASED_DEATH      = 12,
    };

    enum MemberFlags : uint8
    {
        MEMBER_FROZEN                  = 0x01,
        MEMBER_COMMANDABLE_BY_RECIPIENT = 0x02,
        MEMBER_REAL_HUMAN              = 0x04,
        MEMBER_ANCHOR_BODY             = 0x08,
    };

    enum QueueOperation : uint8
    {
        QUEUE_ENQUEUE = 0,
        QUEUE_CANCEL  = 1,
        QUEUE_CLEAR   = 2,
    };

    enum ActionKind : uint8
    {
        ACTION_MOVE   = 1,
        ACTION_ATTACK = 2,
        ACTION_CAST   = 3,
    };

    enum QueueResult : uint8
    {
        QUEUE_OK                    = 0,
        QUEUE_BAD_PACKET            = 1,
        QUEUE_LOCK_NOT_FOUND        = 2,
        QUEUE_NOT_OWNER             = 3,
        QUEUE_LOCK_NOT_ACTIVE       = 4,
        QUEUE_ACTOR_NOT_MEMBER      = 5,
        QUEUE_ACTOR_NOT_COMMANDABLE = 6,
        QUEUE_ACTOR_UNAVAILABLE     = 7,
        QUEUE_FULL                  = 8,
        QUEUE_ACTION_INVALID        = 9,
        QUEUE_ACTION_NOT_FOUND      = 10,
        QUEUE_ACTION_STARTED        = 11,
        QUEUE_ACTION_COMPLETED      = 12,
        QUEUE_ACTION_SKIPPED_INVALID = 13,
        QUEUE_DRAINED               = 14,
    };

    struct QueueRecord
    {
        ObjectGuid actorGuid;
        uint32 actionId = 0;
        uint8 actionKind = 0;
        ObjectGuid targetGuid;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32 spellId = 0;
    };

    void HandleFreeze(WorldSession* session, bool exactSize, uint8 version,
        uint32 requestId, uint8 desiredActive, uint64 lockId);
    void HandleQueue(WorldSession* session, bool exactSize, uint8 version,
        uint64 lockId, uint32 requestId, uint8 operation,
        std::vector<QueueRecord> const& records);

    // Called once from Map::Update on the map thread, before actor updates.
    // It latches newly-entered Units and advances thawed action queues.
    void UpdateMap(Map* map);

    // Called immediately after a Unit advances synchronous spline/motion state.
    // Returns true when the Unit is now held, so its derived update can stop
    // before AI or any other post-motion gameplay work runs in the same tick.
    bool LatchEntrant(Unit* unit);

    // True while an inactive lock owned by this real session still has queued
    // work.  A later freeze is legal, but live input remains fenced until all
    // owned plans drain in deterministic lock/action order.
    bool IsSessionPlanDraining(WorldSession const* session);

    // Gameplay input stays connected but is ignored while either the real
    // session body/currently-driven SUI body is in a field OR an owned plan is
    // draining. HandleFreeze uses the narrower body-only test so reacquire is
    // legal while prior work is serialized.
    bool IsSessionGameplayFrozen(WorldSession const* session);

    // Target-local interaction fence. Resolves a Unit on the acting body's map
    // (or a player-owned corpse back to that player) and reports whether the
    // explicit/stored service source is held by any tactical field.
    bool IsInteractionTargetFrozen(WorldSession* session, ObjectGuid guid);

    // Sealed-boundary predicate shared by spell/damage/AoE seams.
    bool BlocksEffect(Unit const* source, Unit const* target);

    // Lifecycle edges.  Only the socket owner or its driven/anchor body tears
    // down the whole lock; ordinary members cannot thaw somebody else's lock.
    void ReleaseOwnedBy(WorldSession* session, FreezeResult reason);
    void ReleaseForPlayer(Player* player, FreezeResult reason);
    void OnUnitRemoved(Unit* unit);
}

#endif
