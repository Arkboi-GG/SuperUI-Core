#include "SuiTacticalFreeze.h"

#include "AiBotAIMain.h"
#include "CellImpl.h"
#include "GridDefines.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "Objects/Corpse.h"
#include "Objects/Player.h"
#include "Server/WorldSession.h"
#include "SpellMgr.h"
#include "SuiCompanion.h"
#include "SuiPossess.h"
#include "World.h"
#include "WorldPacket.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace
{
    using namespace SuiTacticalFreeze;

    struct QueuedAction
    {
        QueueRecord wire;
        bool issued = false;
        uint32 firstAttemptMs = 0;
        uint32 issuedMs = 0;
    };

    struct ActorQueue
    {
        std::deque<QueuedAction> actions;
        bool manualOwned = false;
        bool previousManual = false;
        bool previousRtsHold = false;
    };

    struct LockState
    {
        uint64 id = 0;
        uint64 ownerGuid = 0;             // real socketed player; authorization identity
        uint64 anchorGuid = 0;            // session->GetSuiActor(); center and member bit 3
        uint32 mapId = 0;
        uint32 instanceId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float radius = RADIUS_YARDS;
        uint32 revision = 1;
        uint32 queueRevision = 1;
        uint32 nextActionId = 0;
        bool active = true;
        bool capLogged = false;
        bool capacityExceeded = false;
        std::set<uint64> members;
        std::set<uint64> freezeObservers;
        std::map<uint64, ActorQueue> queues;
    };

    std::recursive_mutex s_lockMutex;
    std::map<uint64, LockState> s_locks;
    std::map<uint64, uint64> s_ownerActiveLock;
    uint64 s_nextLockId = 0;

    uint64 Raw(ObjectGuid guid)
    {
        return guid.GetRawValue();
    }

    Map* FindLockMap(LockState const& lock)
    {
        return sMapMgr.FindMap(lock.mapId, lock.instanceId);
    }

    Unit* ResolveUnit(LockState const& lock, uint64 rawGuid)
    {
        Map* map = FindLockMap(lock);
        return map ? map->GetUnit(ObjectGuid(rawGuid)) : nullptr;
    }

    Player* ResolveOwner(LockState const& lock)
    {
        Player* owner = sObjectMgr.GetPlayer(ObjectGuid(lock.ownerGuid));
        return owner && owner->IsInWorld() ? owner : nullptr;
    }

    bool IsRealHuman(Player const* player)
    {
        return player && player->GetSession() && !player->GetSession()->GetBot();
    }

    bool IsCommandable(LockState const& lock, Player* owner, Unit* unit)
    {
        Player* actor = unit ? unit->ToPlayer() : nullptr;
        if (!owner || !actor || !actor->IsInWorld() || !actor->IsAlive())
            return false;
        if (actor->GetMapId() != lock.mapId || actor->GetInstanceId() != lock.instanceId)
            return false;
        if (Raw(actor->GetObjectGuid()) != lock.ownerGuid)
        {
            // A second real person's attended character is frozen but read-only.
            if (IsRealHuman(actor))
                return false;
            Group* group = owner->GetGroup();
            if (!group || actor->GetGroup() != group)
                return false;
            if (!SuiCompanion::MayCommand(owner, actor))
                return false;
        }
        // The free-view owner and every orderable bot have the same AiBot executor.
        return dynamic_cast<AiBotAI*>(actor->AI()) != nullptr;
    }

    std::set<WorldSession*> FreezeRecipients(LockState const& lock)
    {
        std::set<WorldSession*> result;
        Map* map = FindLockMap(lock);
        if (!map)
            return result;
        // Observer consistency: an SUI client just outside the radius still
        // renders these actors.  Every real SUI session in the same map/instance
        // receives the geometry and membership/tombstone; bit 1 remains
        // recipient-relative and therefore clear for all nonowners.
        for (auto itr = map->GetPlayers().getFirst(); itr != nullptr; itr = itr->next())
        {
            Player* player = itr->getSource();
            if (!IsRealHuman(player))
                continue;
            WorldSession* session = player->GetSession();
            if (session->IsSuiCapable())
                result.insert(session);
        }
        return result;
    }

    std::set<uint64> FreezeObserverGuids(LockState const& lock)
    {
        std::set<uint64> result;
        for (WorldSession* session : FreezeRecipients(lock))
            if (session->GetPlayer())
                result.insert(Raw(session->GetPlayer()->GetObjectGuid()));
        return result;
    }

    void SendFreezeDenial(WorldSession* session, uint32 requestId, FreezeResult result,
        uint64 requestedLockId = 0)
    {
        if (!session || !session->IsSuiCapable())
            return;
        WorldPacket data(SMSG_SUI_TACTICAL_FREEZE, 45);
        data << uint8(WIRE_VERSION) << uint32(requestId) << uint8(result) << uint8(0);
        data << uint32(0) << uint64(requestedLockId) << uint64(0);
        data << float(0.0f) << float(0.0f) << float(0.0f) << float(0.0f);
        data << uint16(0);
        MANGOS_ASSERT(data.size() == 45);
        session->SendPacket(&data);
    }

    void SendFreezeSnapshotTo(LockState const& lock, WorldSession* recipient,
        uint32 requestId, FreezeResult result, bool active)
    {
        if (!recipient || !recipient->IsSuiCapable())
            return;
        uint16 count = active ? uint16(lock.members.size()) : 0;
        WorldPacket data(SMSG_SUI_TACTICAL_FREEZE, 45u + 9u * count);
        data << uint8(WIRE_VERSION) << uint32(requestId) << uint8(result) << uint8(active ? 1 : 0);
        data << uint32(lock.revision) << uint64(lock.id) << uint64(lock.ownerGuid);
        data << float(active ? lock.x : 0.0f) << float(active ? lock.y : 0.0f)
             << float(active ? lock.z : 0.0f) << float(active ? lock.radius : 0.0f);
        data << uint16(count);
        if (active)
        {
            Player* owner = ResolveOwner(lock);
            bool const ownerRecipient = recipient->GetPlayer() &&
                Raw(recipient->GetPlayer()->GetObjectGuid()) == lock.ownerGuid;
            for (uint64 rawGuid : lock.members)
            {
                Unit* unit = ResolveUnit(lock, rawGuid);
                uint8 flags = MEMBER_FROZEN;
                if (unit)
                {
                    if (Player* player = unit->ToPlayer())
                        if (IsRealHuman(player))
                            flags |= MEMBER_REAL_HUMAN;
                    if (ownerRecipient && IsCommandable(lock, owner, unit))
                        flags |= MEMBER_COMMANDABLE_BY_RECIPIENT;
                }
                if (rawGuid == lock.anchorGuid)
                    flags |= MEMBER_ANCHOR_BODY;
                data << uint64(rawGuid) << uint8(flags);
            }
        }
        MANGOS_ASSERT(data.size() == 45u + 9u * count);
        recipient->SendPacket(&data);
    }

    void BroadcastFreeze(LockState& lock, uint32 requestId,
        FreezeResult result, bool active)
    {
        for (WorldSession* session : FreezeRecipients(lock))
            SendFreezeSnapshotTo(lock, session,
                session->GetPlayer() && Raw(session->GetPlayer()->GetObjectGuid()) == lock.ownerGuid
                    ? requestId : 0,
                result, active);
        lock.freezeObservers = FreezeObserverGuids(lock);
    }

    void SendQueueDenial(WorldSession* session, uint64 lockId, uint32 requestId,
        QueueResult result, uint64 actorGuid = 0, uint32 actionId = 0,
        uint32 revision = 0)
    {
        if (!session || !session->IsSuiCapable())
            return;
        WorldPacket data(SMSG_SUI_TACTICAL_QUEUE, 31);
        data << uint8(WIRE_VERSION) << uint64(lockId) << uint32(revision) << uint32(requestId);
        data << uint8(result) << uint64(actorGuid) << uint32(actionId) << uint8(0);
        MANGOS_ASSERT(data.size() == 31);
        session->SendPacket(&data);
    }

    void SendQueueSnapshotTo(LockState const& lock, WorldSession* session,
        uint32 requestId, QueueResult result, uint64 actorGuid, uint32 actionId)
    {
        if (!session || !session->IsSuiCapable())
            return;
        size_t actionCount = 0;
        for (auto const& pair : lock.queues)
            actionCount += pair.second.actions.size();
        MANGOS_ASSERT(lock.queues.size() <= MAX_REQUEST_RECORDS);
        WorldPacket data(SMSG_SUI_TACTICAL_QUEUE,
            31u + 9u * lock.queues.size() + 29u * actionCount);
        data << uint8(WIRE_VERSION) << uint64(lock.id) << uint32(lock.queueRevision)
             << uint32(requestId) << uint8(result) << uint64(actorGuid)
             << uint32(actionId) << uint8(lock.queues.size());
        for (auto const& pair : lock.queues)
        {
            data << uint64(pair.first) << uint8(pair.second.actions.size());
            for (QueuedAction const& action : pair.second.actions)
            {
                QueueRecord const& row = action.wire;
                data << uint32(row.actionId) << uint8(row.actionKind)
                     << uint64(Raw(row.targetGuid)) << float(row.x) << float(row.y)
                     << float(row.z) << uint32(row.spellId);
            }
        }
        MANGOS_ASSERT(data.size() == 31u + 9u * lock.queues.size() + 29u * actionCount);
        session->SendPacket(&data);
    }

    void BroadcastQueue(LockState const& lock, uint32 requestId,
        QueueResult result, uint64 actorGuid = 0, uint32 actionId = 0)
    {
        // Queues are private command state. Frozen humans and map observers receive
        // freeze snapshots but never another owner's planned actions.
        if (Player* owner = ResolveOwner(lock))
            if (WorldSession* session = owner->GetSession())
                if (!session->GetBot() && session->IsSuiCapable())
                    SendQueueSnapshotTo(lock, session, requestId, result, actorGuid, actionId);
    }

    struct FixedRadiusCheck
    {
        FixedRadiusCheck(Map* map, float x, float y, float z, float radius)
            : map(map), x(x), y(y), z(z), radiusSq(radius * radius) {}

        bool operator()(Unit* unit) const
        {
            if (!unit || !unit->IsInWorld() || !unit->IsAlive() || unit->GetMap() != map)
                return false;
            // The registered freecam eye is infrastructure, not an actor.  It
            // must keep following CMSG_SUI_CAM so visibility/grid streaming
            // remains live while every gameplay Unit in the field is frozen.
            if (SuiPossess::IsFreecamEye(unit))
                return false;
            float const dx = unit->GetPositionX() - x;
            float const dy = unit->GetPositionY() - y;
            float const dz = unit->GetPositionZ() - z;
            return dx * dx + dy * dy + dz * dz <= radiusSq;
        }

        Map* map;
        float x;
        float y;
        float z;
        float radiusSq;
    };

    std::vector<Unit*> UnitsInside(LockState const& lock, Map* map)
    {
        std::list<Unit*> found;
        FixedRadiusCheck check(map, lock.x, lock.y, lock.z, lock.radius);
        MaNGOS::UnitListSearcher<FixedRadiusCheck> searcher(found, check);
        // Never load a grid merely because a freeze exists.  Every in-world actor in
        // already active cells is found, including invisible/hostile actors.
        Cell::VisitAllObjects(lock.x, lock.y, map, searcher, lock.radius, true);
        std::vector<Unit*> units(found.begin(), found.end());
        std::sort(units.begin(), units.end(), [](Unit* left, Unit* right)
        {
            return Raw(left->GetObjectGuid()) < Raw(right->GetObjectGuid());
        });
        units.erase(std::unique(units.begin(), units.end()), units.end());
        return units;
    }

    void PrepareMemberForFreeze(Unit* unit)
    {
        if (!unit)
            return;
        // A counterparty could otherwise commit a trade this player accepted
        // before entering the field. Closing it is cleanup, not a new action.
        if (Player* player = unit->ToPlayer())
            if (player->GetTradeData())
                player->TradeCancel(true);
        unit->AddSuiTacticalFreeze();
    }

    bool LatchInitialMembers(LockState& lock, Map* map)
    {
        std::vector<Unit*> units = UnitsInside(lock, map);
        if (units.size() > MAX_MEMBERS)
            return false;
        // Preflight every fallible invariant before trade cleanup/refcounts:
        // an acquisition denial must not leave irreversible side effects.
        if (std::none_of(units.begin(), units.end(), [&lock](Unit* unit)
            { return Raw(unit->GetObjectGuid()) == lock.anchorGuid; }))
            return false;
        for (Unit* unit : units)
        {
            lock.members.insert(Raw(unit->GetObjectGuid()));
            PrepareMemberForFreeze(unit);
        }
        return true;
    }

    bool LatchEntrants(LockState& lock, Map* map)
    {
        std::vector<Unit*> units = UnitsInside(lock, map);
        size_t newMemberCount = 0;
        for (Unit* unit : units)
            if (!lock.members.count(Raw(unit->GetObjectGuid())))
                ++newMemberCount;
        if (newMemberCount > MAX_MEMBERS - lock.members.size())
        {
            lock.capacityExceeded = true;
            if (!lock.capLogged)
            {
                lock.capLogged = true;
                sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                    "[SUI-FREEZE] lock " UI64FMTD
                    " exceeded the u16 wire member ceiling; thawing without a partial field",
                    lock.id);
            }
            return false;
        }

        bool changed = false;
        for (Unit* unit : units)
        {
            uint64 const guid = Raw(unit->GetObjectGuid());
            if (lock.members.find(guid) != lock.members.end())
                continue;
            lock.members.insert(guid);
            PrepareMemberForFreeze(unit);
            changed = true;
        }
        if (changed)
            ++lock.revision;
        return changed;
    }

    void RestoreManual(Player* actor, ActorQueue& queue)
    {
        if (!queue.manualOwned || !actor)
            return;
        if (AiBotAI* ai = dynamic_cast<AiBotAI*>(actor->AI()))
        {
            ai->m_suiManual = queue.previousManual;
            ai->m_suiRtsHold = queue.previousRtsHold;
        }
        queue.manualOwned = false;
    }

    bool EnsureManual(Player* actor, ActorQueue& queue)
    {
        AiBotAI* ai = actor ? dynamic_cast<AiBotAI*>(actor->AI()) : nullptr;
        if (!ai)
            return false;
        if (!queue.manualOwned)
        {
            queue.previousManual = ai->m_suiManual;
            queue.previousRtsHold = ai->m_suiRtsHold;
            queue.manualOwned = true;
        }
        ai->m_suiManual = true;
        ai->m_suiRtsHold = true;
        return true;
    }

    bool IsZero(float value)
    {
        return value == 0.0f;
    }

    bool IsZeroActionPayload(QueueRecord const& row)
    {
        return row.actionKind == 0 && row.targetGuid.IsEmpty() &&
            IsZero(row.x) && IsZero(row.y) && IsZero(row.z) && row.spellId == 0;
    }

    bool ValidateEnqueuePayload(LockState const& lock, Player* actor, QueueRecord const& row)
    {
        if (row.actionId != 0 || !actor || !std::isfinite(row.x) ||
            !std::isfinite(row.y) || !std::isfinite(row.z))
            return false;
        switch (row.actionKind)
        {
            case ACTION_MOVE:
                return row.targetGuid.IsEmpty() && row.spellId == 0 &&
                    MaNGOS::IsValidMapCoord(row.x, row.y, row.z);
            case ACTION_ATTACK:
                return !row.targetGuid.IsEmpty() && row.spellId == 0 &&
                    IsZero(row.x) && IsZero(row.y) && IsZero(row.z) &&
                    actor->GetMap()->GetUnit(row.targetGuid) != nullptr;
            case ACTION_CAST:
            {
                SpellEntry const* spell = sSpellMgr.GetSpellEntry(row.spellId);
                if (!spell || !row.spellId || !actor->HasActiveSpell(row.spellId) ||
                    spell->IsPassiveSpell() || spell->IsAutoRepeatRangedSpell())
                    return false;
                if (!row.targetGuid.IsEmpty())
                    return IsZero(row.x) && IsZero(row.y) && IsZero(row.z) &&
                        actor->GetMap()->GetUnit(row.targetGuid) != nullptr;
                return MaNGOS::IsValidMapCoord(row.x, row.y, row.z);
            }
            default:
                return false;
        }
    }

    void ReleaseLock(uint64 lockId, FreezeResult reason)
    {
        auto itr = s_locks.find(lockId);
        if (itr == s_locks.end() || !itr->second.active)
            return;
        LockState& lock = itr->second;
        lock.active = false;
        ++lock.revision;
        s_ownerActiveLock.erase(lock.ownerGuid);
        for (uint64 rawGuid : lock.members)
            if (Unit* unit = ResolveUnit(lock, rawGuid))
                unit->RemoveSuiTacticalFreeze();

        sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
            "[SUI-FREEZE] released lock " UI64FMTD " owner " UI64FMTD " reason %u members %u queues %u",
            lock.id, lock.ownerGuid, uint32(reason), uint32(lock.members.size()), uint32(lock.queues.size()));
        BroadcastFreeze(lock, 0, reason, false);
        if (lock.queues.empty())
            s_locks.erase(itr);
    }

    bool CompleteIssuedAction(LockState& lock, Player* actor, ActorQueue& queue,
        QueuedAction const& action, uint32 now, bool& invalid)
    {
        invalid = false;
        switch (action.wire.actionKind)
        {
            case ACTION_MOVE:
                if (actor->GetDistance3dToCenter(action.wire.x, action.wire.y, action.wire.z) <= 3.5f)
                    return true;
                if (uint32(now - action.issuedMs) > 120000)
                    invalid = true;
                return false;
            case ACTION_ATTACK:
            {
                Unit* target = actor->GetMap()->GetUnit(action.wire.targetGuid);
                if (!target || !target->IsAlive() || actor->GetVictim() == target)
                    return true;
                if (uint32(now - action.issuedMs) > 10000)
                    invalid = true;
                return false;
            }
            case ACTION_CAST:
                if (!actor->IsNonMeleeSpellCasted(false))
                    return true;
                if (uint32(now - action.issuedMs) > 30000)
                {
                    actor->InterruptNonMeleeSpells(false);
                    invalid = true;
                }
                return false;
            default:
                invalid = true;
                return false;
        }
    }

    QueueResult IssueAction(LockState const& lock, Player* actor, ActorQueue& queue,
        QueuedAction& action, uint32 now)
    {
        if (!EnsureManual(actor, queue))
            return QUEUE_ACTION_SKIPPED_INVALID;

        // Overlap law: thawing lock A must not consume an Attack/Cast whose
        // explicit target is still held by lock B.  Wait without starting the
        // retry budget; once the target thaws, ordinary validation decides it.
        if (action.wire.actionKind == ACTION_ATTACK ||
            (action.wire.actionKind == ACTION_CAST && !action.wire.targetGuid.IsEmpty()))
        {
            Unit* target = actor->GetMap()->GetUnit(action.wire.targetGuid);
            if (target && target->IsSuiTacticallyFrozen())
                return QUEUE_OK;
        }
        if (!action.firstAttemptMs)
            action.firstAttemptMs = now;

        switch (action.wire.actionKind)
        {
            case ACTION_MOVE:
            {
                AiBotAI* ai = dynamic_cast<AiBotAI*>(actor->AI());
                float x = action.wire.x, y = action.wire.y, z = action.wire.z;
                if (!ai || !MaNGOS::IsValidMapCoord(x, y, z) || !ai->SuiValidateOrderDest(x, y, z))
                    return QUEUE_ACTION_SKIPPED_INVALID;
                action.wire.x = x;
                action.wire.y = y;
                action.wire.z = z;
                ai->QueueSuiRtsMove(x, y, z);
                break;
            }
            case ACTION_ATTACK:
            {
                AiBotAI* ai = dynamic_cast<AiBotAI*>(actor->AI());
                Unit* target = actor->GetMap()->GetUnit(action.wire.targetGuid);
                if (!ai || !target || target == actor || !target->IsAlive() ||
                    actor->IsFriendlyTo(target) ||
                    target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING | UNIT_FLAG_NOT_SELECTABLE))
                    return QUEUE_ACTION_SKIPPED_INVALID;
                ai->m_suiOrderedAttackPass = true;
                bool const started = ai->AttackStart(target);
                ai->m_suiOrderedAttackPass = false;
                if (!started && actor->GetVictim() != target)
                    return QUEUE_ACTION_SKIPPED_INVALID;
                break;
            }
            case ACTION_CAST:
            {
                SpellEntry const* spell = sSpellMgr.GetSpellEntry(action.wire.spellId);
                if (!spell || !actor->HasActiveSpell(action.wire.spellId) ||
                    spell->IsPassiveSpell() || spell->IsAutoRepeatRangedSpell())
                    return QUEUE_ACTION_SKIPPED_INVALID;
                // A valid second queued cast waits for the first cast's GCD/cast bar.
                if (actor->IsNonMeleeSpellCasted(false) || !actor->IsSpellReady(*spell) || actor->HasGCD(spell))
                {
                    if (uint32(now - action.firstAttemptMs) > 30000)
                        return QUEUE_ACTION_SKIPPED_INVALID;
                    return QUEUE_OK; // retry, not yet issued
                }
                // A cast supersedes the motion sampled at freeze.  AiBotAI's
                // stop helper brackets MotionMaster::Clear with its false-
                // arrival suppression latch; it does not stop autoattack.
                AiBotAI* ai = dynamic_cast<AiBotAI*>(actor->AI());
                if (!ai)
                    return QUEUE_ACTION_SKIPPED_INVALID;
                ai->StopMoving();
                SpellCastResult result = SPELL_FAILED_BAD_TARGETS;
                if (!action.wire.targetGuid.IsEmpty())
                {
                    Unit* target = actor->GetMap()->GetUnit(action.wire.targetGuid);
                    if (!target)
                        return QUEUE_ACTION_SKIPPED_INVALID;
                    if (target != actor && !actor->IsFacingTarget(target))
                        actor->SetFacingToObject(target);
                    result = actor->CastSpell(target, spell, false);
                }
                else
                {
                    if (!MaNGOS::IsValidMapCoord(action.wire.x, action.wire.y, action.wire.z))
                        return QUEUE_ACTION_SKIPPED_INVALID;
                    result = actor->CastSpell(action.wire.x, action.wire.y, action.wire.z, spell, false);
                }
                if (result != SPELL_CAST_OK)
                    return QUEUE_ACTION_SKIPPED_INVALID;
                break;
            }
            default:
                return QUEUE_ACTION_SKIPPED_INVALID;
        }

        action.issued = true;
        action.issuedMs = now;
        return QUEUE_ACTION_STARTED;
    }

    bool HasOlderQueuedAction(uint64 lockId, uint64 actorGuid)
    {
        for (auto itr = s_locks.begin(); itr != s_locks.end() && itr->first < lockId; ++itr)
        {
            auto queueItr = itr->second.queues.find(actorGuid);
            if (queueItr != itr->second.queues.end() && !queueItr->second.actions.empty())
                return true;
        }
        return false;
    }

    void UpdateQueueActor(LockState& lock, uint64 actorGuid, ActorQueue& queue, uint32 now)
    {
        if (queue.actions.empty())
            return;
        Player* owner = ResolveOwner(lock);
        Player* actor = nullptr;
        if (Unit* unit = ResolveUnit(lock, actorGuid))
            actor = unit->ToPlayer();
        QueuedAction& action = queue.actions.front();

        if (!owner || !actor || lock.members.find(actorGuid) == lock.members.end() ||
            !IsCommandable(lock, owner, actor))
        {
            uint32 const actionId = action.wire.actionId;
            queue.actions.pop_front();
            ++lock.queueRevision;
            BroadcastQueue(lock, 0, QUEUE_ACTION_SKIPPED_INVALID, actorGuid, actionId);
            return;
        }

        // A later lock may be acquired while this owner has an older plan.
        // Serialize only the actors that overlap; unrelated actors can drain
        // concurrently, while a shared actor observes lock-id then FIFO order.
        if (HasOlderQueuedAction(lock.id, actorGuid))
            return;

        // Command View exit and other hand-back paths may clear m_suiManual.
        // Reassert it on every tick of the oldest plan, including an issued
        // action or a wait behind an overlapping active freeze.
        if (!EnsureManual(actor, queue))
        {
            uint32 const actionId = action.wire.actionId;
            queue.actions.pop_front();
            ++lock.queueRevision;
            BroadcastQueue(lock, 0, QUEUE_ACTION_SKIPPED_INVALID, actorGuid, actionId);
            return;
        }

        // An overlapping active lock is a wait, not an invalidation.
        if (actor->IsSuiTacticallyFrozen())
            return;

        if (action.issued)
        {
            bool invalid = false;
            if (!CompleteIssuedAction(lock, actor, queue, action, now, invalid) && !invalid)
                return;
            uint32 const actionId = action.wire.actionId;
            queue.actions.pop_front();
            ++lock.queueRevision;
            BroadcastQueue(lock, 0,
                invalid ? QUEUE_ACTION_SKIPPED_INVALID : QUEUE_ACTION_COMPLETED,
                actorGuid, actionId);
            return;
        }

        QueueResult result = IssueAction(lock, actor, queue, action, now);
        if (result == QUEUE_OK)
            return;
        if (result == QUEUE_ACTION_SKIPPED_INVALID)
        {
            uint32 const actionId = action.wire.actionId;
            queue.actions.pop_front();
            ++lock.queueRevision;
            BroadcastQueue(lock, 0, result, actorGuid, actionId);
            return;
        }
        ++lock.queueRevision;
        BroadcastQueue(lock, 0, result, actorGuid, action.wire.actionId);
    }
}

namespace SuiTacticalFreeze
{
    bool IsSessionPlanDraining(WorldSession const* session)
    {
        if (!session || !session->GetPlayer())
            return false;
        uint64 const ownerGuid = Raw(session->GetPlayer()->GetObjectGuid());
        std::lock_guard<std::recursive_mutex> guard(s_lockMutex);
        for (auto const& pair : s_locks)
            if (!pair.second.active && pair.second.ownerGuid == ownerGuid &&
                !pair.second.queues.empty())
                return true;
        return false;
    }

    bool IsSessionGameplayFrozen(WorldSession const* session)
    {
        if (!session)
            return false;
        Player* player = session->GetPlayer();
        if (player && player->IsSuiTacticallyFrozen())
            return true;
        Player* actor = const_cast<WorldSession*>(session)->GetSuiActor();
        if (actor && actor->IsSuiTacticallyFrozen())
            return true;
        return IsSessionPlanDraining(session);
    }

    bool IsInteractionTargetFrozen(WorldSession* session, ObjectGuid guid)
    {
        Player* actor = session ? session->GetSuiActor() : nullptr;
        if (!actor || !actor->IsInWorld() || guid.IsEmpty())
            return false;

        // Social/invite/summon packets can name a player on another map.  A
        // tactical member remains a sealed target there too.
        if (guid.IsPlayer())
            if (Player* player = sObjectMgr.GetPlayer(guid))
                return player->IsSuiTacticallyFrozen();

        Map* map = actor->GetMap();
        if (!map)
            return false;
        if (guid.IsUnit())
        {
            Unit* target = map->GetUnit(guid);
            return target && target->IsSuiTacticallyFrozen();
        }
        if (guid.IsCorpse())
        {
            Corpse* corpse = map->GetCorpse(guid);
            if (!corpse || corpse->GetOwnerGuid().IsEmpty())
                return false;
            Player* owner = sObjectMgr.GetPlayer(corpse->GetOwnerGuid());
            return owner && owner->IsSuiTacticallyFrozen();
        }
        return false;
    }

    bool BlocksEffect(Unit const* source, Unit const* target)
    {
        return (source && source->IsSuiTacticallyFrozen()) ||
            (target && target->IsSuiTacticallyFrozen());
    }

    void HandleFreeze(WorldSession* session, bool exactSize, uint8 version,
        uint32 requestId, uint8 desiredActive, uint64 lockId)
    {
        if (!session)
            return;
        session->SetSuiCapable(true);
        std::lock_guard<std::recursive_mutex> guard(s_lockMutex);
        if (!exactSize || version != WIRE_VERSION || !requestId || desiredActive > 1 ||
            (desiredActive && lockId != 0) || (!desiredActive && lockId == 0))
        {
            SendFreezeDenial(session, requestId, FREEZE_BAD_PACKET);
            return;
        }

        Player* owner = session->GetPlayer();
        if (!owner || session->GetBot() || !owner->IsInWorld())
        {
            SendFreezeDenial(session, requestId, FREEZE_DENIED_SESSION);
            return;
        }

        if (!desiredActive)
        {
            auto itr = s_locks.find(lockId);
            if (itr == s_locks.end() || !itr->second.active)
            {
                SendFreezeDenial(session, requestId, FREEZE_NOT_FOUND, lockId);
                return;
            }
            LockState& lock = itr->second;
            if (lock.ownerGuid != Raw(owner->GetObjectGuid()))
            {
                SendFreezeSnapshotTo(lock, session, requestId, FREEZE_NOT_OWNER, true);
                return;
            }
            ++lock.revision;
            // The request response is the tombstone; ReleaseLock broadcasts it with requestId 0.
            SendFreezeSnapshotTo(lock, session, requestId, FREEZE_OK, false);
            --lock.revision;
            ReleaseLock(lockId, FREEZE_OK);
            return;
        }

        uint64 const ownerGuid = Raw(owner->GetObjectGuid());
        auto activeItr = s_ownerActiveLock.find(ownerGuid);
        if (activeItr != s_ownerActiveLock.end())
        {
            auto lockItr = s_locks.find(activeItr->second);
            if (lockItr != s_locks.end() && lockItr->second.active)
                SendFreezeSnapshotTo(lockItr->second, session, requestId, FREEZE_ALREADY_ACTIVE, true);
            else
                SendFreezeDenial(session, requestId, FREEZE_DENIED_STATE);
            return;
        }

        // A prior owned plan may still be draining; reacquire remains legal
        // and per-actor execution is serialized by HasOlderQueuedAction.
        bool bodyFrozen = owner->IsSuiTacticallyFrozen();
        if (Player* driven = session->GetSuiActor())
            bodyFrozen = bodyFrozen || driven->IsSuiTacticallyFrozen();
        if (bodyFrozen)
        {
            for (auto const& pair : s_locks)
            {
                LockState const& candidate = pair.second;
                if (!candidate.active)
                    continue;
                uint64 const bodyGuid = ownerGuid;
                Player* driven = session->GetSuiActor();
                uint64 const drivenGuid = driven ? Raw(driven->GetObjectGuid()) : 0;
                if (candidate.members.count(bodyGuid) || candidate.members.count(drivenGuid))
                {
                    SendFreezeSnapshotTo(candidate, session, requestId, FREEZE_FROZEN_BY_OTHER, true);
                    return;
                }
            }
            SendFreezeDenial(session, requestId, FREEZE_DENIED_STATE);
            return;
        }

        if (!SuiPossess::IsFreeViewUp(owner))
        {
            SendFreezeDenial(session, requestId, FREEZE_DENIED_COMMAND_VIEW);
            return;
        }

        Player* anchor = session->GetSuiActor();
        bool const controlled = anchor == owner || SuiPossess::GetPossessor(anchor) == owner;
        if (!anchor || !controlled || !anchor->IsInWorld() || !anchor->IsAlive() ||
            anchor->GetMap() != owner->GetMap() || anchor->IsTaxiFlying() ||
            anchor->GetTransport() || anchor->IsBeingTeleported())
        {
            SendFreezeDenial(session, requestId, FREEZE_DENIED_STATE);
            return;
        }

        LockState lock;
        do { lock.id = ++s_nextLockId; } while (!lock.id || s_locks.count(lock.id));
        lock.ownerGuid = ownerGuid;
        lock.anchorGuid = Raw(anchor->GetObjectGuid());
        lock.mapId = anchor->GetMapId();
        lock.instanceId = anchor->GetInstanceId();
        lock.x = anchor->GetPositionX();
        lock.y = anchor->GetPositionY();
        lock.z = anchor->GetPositionZ();
        if (!LatchInitialMembers(lock, anchor->GetMap()))
        {
            for (uint64 rawGuid : lock.members)
                if (Unit* unit = ResolveUnit(lock, rawGuid))
                    unit->RemoveSuiTacticalFreeze();
            SendFreezeDenial(session, requestId, FREEZE_DENIED_STATE);
            return;
        }

        uint64 const newId = lock.id;
        s_locks.emplace(newId, std::move(lock));
        s_ownerActiveLock[ownerGuid] = newId;
        LockState& created = s_locks.find(newId)->second;
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC,
            "[SUI-FREEZE] acquired lock " UI64FMTD " owner " UI64FMTD " anchor " UI64FMTD
            " map %u:%u center %.2f %.2f %.2f members %u",
            created.id, created.ownerGuid, created.anchorGuid, created.mapId,
            created.instanceId, created.x, created.y, created.z, uint32(created.members.size()));
        BroadcastFreeze(created, requestId, FREEZE_OK, true);
    }

    void HandleQueue(WorldSession* session, bool exactSize, uint8 version,
        uint64 lockId, uint32 requestId, uint8 operation,
        std::vector<QueueRecord> const& records)
    {
        if (!session)
            return;
        session->SetSuiCapable(true);
        std::lock_guard<std::recursive_mutex> guard(s_lockMutex);
        if (!exactSize || version != WIRE_VERSION || !requestId || records.empty() ||
            records.size() > MAX_REQUEST_RECORDS || operation > QUEUE_CLEAR)
        {
            SendQueueDenial(session, lockId, requestId, QUEUE_BAD_PACKET);
            return;
        }
        auto lockItr = s_locks.find(lockId);
        if (lockItr == s_locks.end())
        {
            SendQueueDenial(session, lockId, requestId, QUEUE_LOCK_NOT_FOUND);
            return;
        }
        LockState& lock = lockItr->second;
        Player* owner = session->GetPlayer();
        if (!owner || session->GetBot() || Raw(owner->GetObjectGuid()) != lock.ownerGuid)
        {
            SendQueueDenial(session, lockId, requestId, QUEUE_NOT_OWNER, 0, 0,
                lock.queueRevision);
            return;
        }
        if (!lock.active)
        {
            SendQueueSnapshotTo(lock, session, requestId, QUEUE_LOCK_NOT_ACTIVE, 0, 0);
            return;
        }

        std::set<uint64> actors;
        size_t prospectiveNewQueues = 0;
        for (QueueRecord const& row : records)
        {
            uint64 const actorGuid = Raw(row.actorGuid);
            if (!actorGuid || !actors.insert(actorGuid).second)
            {
                // BAD_PACKET is deliberately stateless in v1.  Do not attach
                // the live queue revision/body to malformed input.
                SendQueueDenial(session, lockId, requestId, QUEUE_BAD_PACKET);
                return;
            }
            if (!lock.members.count(actorGuid))
            {
                SendQueueSnapshotTo(lock, session, requestId, QUEUE_ACTOR_NOT_MEMBER, actorGuid, row.actionId);
                return;
            }
            Player* actor = nullptr;
            if (Unit* unit = ResolveUnit(lock, actorGuid))
                actor = unit->ToPlayer();
            if (!actor)
            {
                SendQueueSnapshotTo(lock, session, requestId, QUEUE_ACTOR_UNAVAILABLE, actorGuid, row.actionId);
                return;
            }
            if (!IsCommandable(lock, owner, actor))
            {
                SendQueueSnapshotTo(lock, session, requestId, QUEUE_ACTOR_NOT_COMMANDABLE, actorGuid, row.actionId);
                return;
            }

            auto queueItr = lock.queues.find(actorGuid);
            if (operation == QUEUE_ENQUEUE)
            {
                if (!ValidateEnqueuePayload(lock, actor, row))
                {
                    SendQueueSnapshotTo(lock, session, requestId, QUEUE_ACTION_INVALID, actorGuid, row.actionId);
                    return;
                }
                size_t const size = queueItr == lock.queues.end() ? 0 : queueItr->second.actions.size();
                if (queueItr == lock.queues.end())
                    ++prospectiveNewQueues;
                if (size >= MAX_ACTIONS_PER_ACTOR ||
                    lock.queues.size() + prospectiveNewQueues > MAX_REQUEST_RECORDS)
                {
                    SendQueueSnapshotTo(lock, session, requestId, QUEUE_FULL, actorGuid, row.actionId);
                    return;
                }
            }
            else if (operation == QUEUE_CANCEL)
            {
                if (!row.actionId || !IsZeroActionPayload(row) || queueItr == lock.queues.end() ||
                    std::none_of(queueItr->second.actions.begin(), queueItr->second.actions.end(),
                        [&row](QueuedAction const& action) { return action.wire.actionId == row.actionId; }))
                {
                    SendQueueSnapshotTo(lock, session, requestId, QUEUE_ACTION_NOT_FOUND, actorGuid, row.actionId);
                    return;
                }
            }
            else if (row.actionId || !IsZeroActionPayload(row))
            {
                SendQueueSnapshotTo(lock, session, requestId, QUEUE_ACTION_INVALID, actorGuid, row.actionId);
                return;
            }
        }

        uint64 resultActor = records.size() == 1 ? Raw(records.front().actorGuid) : 0;
        uint32 resultAction = 0;
        for (QueueRecord const& row : records)
        {
            uint64 const actorGuid = Raw(row.actorGuid);
            if (operation == QUEUE_ENQUEUE)
            {
                QueuedAction action;
                action.wire = row;
                do { action.wire.actionId = ++lock.nextActionId; } while (!action.wire.actionId);
                resultAction = records.size() == 1 ? action.wire.actionId : 0;
                lock.queues[actorGuid].actions.push_back(action);
            }
            else if (operation == QUEUE_CANCEL)
            {
                ActorQueue& queue = lock.queues.find(actorGuid)->second;
                auto actionItr = std::find_if(queue.actions.begin(), queue.actions.end(),
                    [&row](QueuedAction const& action) { return action.wire.actionId == row.actionId; });
                queue.actions.erase(actionItr);
                resultAction = records.size() == 1 ? row.actionId : 0;
                if (queue.actions.empty())
                    lock.queues.erase(actorGuid);
            }
            else
                lock.queues.erase(actorGuid);
        }
        ++lock.queueRevision;
        BroadcastQueue(lock, requestId, QUEUE_OK, resultActor, resultAction);
    }

    bool LatchEntrant(Unit* unit)
    {
        if (!unit || !unit->IsInWorld() || !unit->IsAlive() ||
            SuiPossess::IsFreecamEye(unit))
            return false;

        Map* map = unit->GetMap();
        if (!map)
            return false;

        std::lock_guard<std::recursive_mutex> guard(s_lockMutex);
        std::vector<uint64> emergencyRelease;
        uint64 const unitGuid = Raw(unit->GetObjectGuid());
        for (auto& pair : s_locks)
        {
            LockState& lock = pair.second;
            if (!lock.active || lock.mapId != map->GetId() ||
                lock.instanceId != map->GetInstanceId() ||
                lock.members.find(unitGuid) != lock.members.end())
                continue;
            if (!FixedRadiusCheck(map, lock.x, lock.y, lock.z, lock.radius)(unit))
                continue;

            // Never form a partial field when the natural u16 wire ceiling is
            // exhausted. The whole overflowing lock is released after this
            // iteration; overlap references from other locks remain intact.
            if (lock.members.size() >= MAX_MEMBERS)
            {
                lock.capacityExceeded = true;
                if (!lock.capLogged)
                {
                    lock.capLogged = true;
                    sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                        "[SUI-FREEZE] lock " UI64FMTD
                        " exceeded the u16 wire member ceiling during post-motion latching; thawing without a partial field",
                        lock.id);
                }
                emergencyRelease.push_back(lock.id);
                continue;
            }

            lock.members.insert(unitGuid);
            PrepareMemberForFreeze(unit);
            ++lock.revision;
            BroadcastFreeze(lock, 0, FREEZE_OK, true);
        }

        for (uint64 lockId : emergencyRelease)
            ReleaseLock(lockId, FREEZE_RELEASED_MAP_CHANGE);
        return unit->IsSuiTacticallyFrozen();
    }

    void UpdateMap(Map* map)
    {
        if (!map)
            return;
        std::lock_guard<std::recursive_mutex> guard(s_lockMutex);
        std::vector<uint64> emergencyRelease;
        for (auto& pair : s_locks)
        {
            LockState& lock = pair.second;
            if (lock.mapId != map->GetId() || lock.instanceId != map->GetInstanceId())
                continue;
            if (lock.active)
            {
                Player* owner = ResolveOwner(lock);
                Unit* anchor = ResolveUnit(lock, lock.anchorGuid);
                if (!owner || !anchor || !anchor->IsAlive() || anchor->GetMap() != map)
                {
                    emergencyRelease.push_back(lock.id);
                    continue;
                }
                bool const membershipChanged = LatchEntrants(lock, map);
                if (lock.capacityExceeded)
                {
                    emergencyRelease.push_back(lock.id);
                    continue;
                }
                bool const observerChanged = FreezeObserverGuids(lock) != lock.freezeObservers;
                if (membershipChanged || observerChanged)
                    BroadcastFreeze(lock, 0, FREEZE_OK, true);
                continue;
            }

            uint32 const now = World::GetCurrentMSTime();
            std::vector<uint64> actorGuids;
            for (auto const& queue : lock.queues)
                actorGuids.push_back(queue.first);
            for (uint64 actorGuid : actorGuids)
            {
                auto queueItr = lock.queues.find(actorGuid);
                if (queueItr == lock.queues.end())
                    continue;
                UpdateQueueActor(lock, actorGuid, queueItr->second, now);
                if (queueItr->second.actions.empty())
                {
                    Player* actor = nullptr;
                    if (Unit* unit = ResolveUnit(lock, actorGuid))
                        actor = unit->ToPlayer();
                    RestoreManual(actor, queueItr->second);
                    lock.queues.erase(queueItr);
                }
            }
        }
        for (uint64 lockId : emergencyRelease)
            ReleaseLock(lockId, FREEZE_RELEASED_MAP_CHANGE);

        for (auto itr = s_locks.begin(); itr != s_locks.end(); )
        {
            LockState& lock = itr->second;
            if (!lock.active && lock.mapId == map->GetId() &&
                lock.instanceId == map->GetInstanceId() && lock.queues.empty())
            {
                ++lock.queueRevision;
                BroadcastQueue(lock, 0, QUEUE_DRAINED);
                itr = s_locks.erase(itr);
            }
            else
                ++itr;
        }
    }

    void ReleaseOwnedBy(WorldSession* session, FreezeResult reason)
    {
        if (!session || !session->GetPlayer())
            return;
        std::lock_guard<std::recursive_mutex> guard(s_lockMutex);
        uint64 const ownerGuid = Raw(session->GetPlayer()->GetObjectGuid());
        auto itr = s_ownerActiveLock.find(ownerGuid);
        if (itr != s_ownerActiveLock.end())
            ReleaseLock(itr->second, reason);
    }

    void ReleaseForPlayer(Player* player, FreezeResult reason)
    {
        if (!player)
            return;
        std::lock_guard<std::recursive_mutex> guard(s_lockMutex);
        uint64 const guid = Raw(player->GetObjectGuid());
        std::vector<uint64> release;
        for (auto const& pair : s_locks)
            if (pair.second.active &&
                (pair.second.ownerGuid == guid || pair.second.anchorGuid == guid))
                release.push_back(pair.first);
        for (uint64 lockId : release)
            ReleaseLock(lockId, reason);
    }

    void OnUnitRemoved(Unit* unit)
    {
        if (!unit)
            return;
        std::lock_guard<std::recursive_mutex> guard(s_lockMutex);
        uint64 const guid = Raw(unit->GetObjectGuid());
        std::vector<uint64> release;
        for (auto& pair : s_locks)
        {
            LockState& lock = pair.second;
            if (!lock.active || !lock.members.count(guid))
                continue;
            if (lock.ownerGuid == guid || lock.anchorGuid == guid)
            {
                release.push_back(lock.id);
                continue;
            }
            lock.members.erase(guid);
            unit->RemoveSuiTacticalFreeze();
            ++lock.revision;
            BroadcastFreeze(lock, 0, FREEZE_OK, true);
        }
        for (uint64 lockId : release)
            ReleaseLock(lockId, FREEZE_RELEASED_MAP_CHANGE);
    }
}
