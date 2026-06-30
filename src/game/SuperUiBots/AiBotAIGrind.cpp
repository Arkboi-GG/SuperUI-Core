/*
 * AiBotAIGrind.cpp — area-grind behaviour for the autonomous AI bot.
 *
 * Split from the monolithic AiBotAI.cpp. THIS TU holds the grind domain:
 *   - CountNearbyHostiles (the isolation/overpull primitive)
 *   - SelectGrindTarget (priority scan: aggroed → objective rescan → indefinite XP mob),
 *     fronted by the [TEAMPLAY] focus-fire seam
 *   - DoGrindPatrol, ScanApproachTarget, ConvertMoveToGrindInPlace
 *
 * The file-local AiBotGrayLevel() helper is defined here (its only caller is
 * SelectGrindTarget, so it stays in this TU). All other definitions are members of
 * AiBotAI and link across the sibling TUs transparently; cross-TU members called from
 * here (ClearStoredPath) live in AiBotAIMovement.cpp.
 */

#include "AiBotAIMain.h"
#include "AiBotAITeamPlay.h"   // [TEAMPLAY] ResolveCombatTarget — the group focus-fire resolver
#include "Player.h"
#include <cstring>
#include <cstdio>
#include <set>
#include "Group.h"
#include "CreatureAI.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "PlayerBotMgr.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "World.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "BattleGround.h"
#include "TargetedMovementGenerator.h"
#include "QuestDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Server/Packets/Channel.h"
#include "ChannelMgr.h"
#include "Bag.h"
#include "PathFinder.h"
#include "MoveMap.h"

// ============================================================
// TASK_GRIND — Area grind behavior (Phase 2.5)
// ============================================================

// ============================================================
// SESSION 31: Count hostile creatures near a target position
//
// Used by SelectGrindTarget to prefer isolated mobs over pack
// centers. Counts alive, hostile, untapped creatures within
// `radius` of the candidate — excluding the candidate itself.
// ============================================================
 
uint32 AiBotAI::CountNearbyHostiles(Unit* pCandidate, float radius) const
{
    if (!pCandidate || !pCandidate->IsAlive())
        return 0;

    uint32 count = 0;
    std::list<Unit*> targets;
    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck u_check(pCandidate, me, radius);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(targets, u_check);
    Cell::VisitAllObjects(pCandidate, searcher, radius);

    for (Unit* pUnit : targets)
    {
        if (pUnit == pCandidate)
            continue;
        if (!pUnit->IsAlive())
            continue;
        if (!pUnit->IsCreature())
            continue;
        if (!IsValidHostileTarget(pUnit))
            continue;
        // FACTION GATE (the neutral fix): only count units that will actually PILE ON when we pull —
        // i.e. reaction-hostile to us (proximity-aggro). IsValidHostileTarget/AnyUnfriendly include
        // NEUTRALS (a player CAN swing at them), but pulling a mob doesn't wake neutral bystanders, so
        // counting them falsely inflated the cluster depth and froze the pull next to killable mobs.
        // (Same-faction social-assist among a tight neutral pack is the one residue — a future one-
        // predicate add here; proximity-aggro is the whole starter-field case.)
        if (!me->IsHostileTo(pUnit))
            continue;
        if (static_cast<Creature*>(pUnit)->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED)
            && !static_cast<Creature*>(pUnit)->IsTappedBy(me))
            continue;

        count++;
    }

    return count;
}



// ============================================================================
// SelectGrindTarget — COMPLETE METHOD REPLACEMENT (2026-06-22)
//
// Only Priority 2b changed (the indefinite kill-anything grind, creatureEntry==0).
// Priority 1 (already-aggroed) and Priority 2a (objective rescan) are VERBATIM.
//
// 2b directive: find the NEAREST mob that is a real XP kill, and kill it; rescan; repeat.
//   - skip CREATURE_TYPE_CRITTER (8)  → no more chicken / sheep / cow farms (0 XP)
//   - skip grey (level <= gray)        → no XP, not worth a swing
//   - skip > L+AIBOT_GRIND_HIGH_OFFSET → don't suicide on a red (an L7 in Westfall finds
//                                        NOTHING valid → no kills → C# breaker hearths it out)
//   - pure NEAREST: no density penalty, no center leash (bot-centric scan to
//                   AIBOT_GRIND_SCAN_YARDS). Chasing a valid mob a little further IS progress.
// ============================================================================

// Vanilla 1.12 grey-level formula (a mob AT OR BELOW this level gives the bot ZERO xp).
// Inlined exact (fork-independent) so the grind never wastes a swing on a no-xp mob.
static uint32 AiBotGrayLevel(uint32 plLevel)
{
    if (plLevel <= 5)  return 0;
    if (plLevel <= 39) return plLevel - 5 - plLevel / 10;
    if (plLevel <= 59) return plLevel - 1 - plLevel / 5;
    return plLevel - 9;
}

Unit* AiBotAI::SelectGrindTarget() const
{
    if (m_currentTask.type != TASK_GRIND)
        return nullptr;

    // [TEAMPLAY] Group focus-fire seam (grind-path entry). A grouped escort's grind target is
    // overridden to the anchor's victim when an assist directive is live, so the team piles one
    // mob instead of each escort fanning to its own nearest. Inactive → null → the existing
    // priority scan below runs byte-for-byte. (Kill-counting is unaffected — UpdateAI still
    // only credits the objective when victim entry == creatureEntry.)
    if (m_combatDirective.IsActive())
        if (Unit* t = TeamPlay::ResolveCombatTarget(*this))
            return t;

    float const AGGRO_PENALTY = 15.0f;

    // ── Priority 1: already-aggroed mobs (both grind modes) ──
    // Anything on our threat list that matches the quest entry (or any hostile,
    // for the kill-anything grind) is handled first — we're already in combat
    // with it. Capped at 50y: an aggroed mob is by definition close.
    {
        float bestDist = 50.0f;
        Unit* bestTarget = nullptr;

        HostileReference* pRef = me->GetHostileRefManager().getFirst();
        while (pRef)
        {
            if (Unit* pTarget = pRef->getSourceUnit())
            {
                if (IsValidHostileTarget(pTarget) &&
                    !IsCombatIgnored(pTarget->GetGUIDLow()) &&
                    me->IsWithinDist(pTarget, bestDist))
                {
                    if (m_currentTask.creatureEntry == 0 ||
                        (pTarget->IsCreature() &&
                         static_cast<Creature*>(pTarget)->GetEntry() == m_currentTask.creatureEntry))
                    {
                        float d = me->GetDistance(pTarget);
                        if (d < bestDist)
                        {
                            bestDist = d;
                            bestTarget = pTarget;
                        }
                    }
                }
            }
            pRef = pRef->next();
        }

        if (bestTarget)
            return bestTarget;
    }

    // Scores a creature list against the BOT and returns the best (lowest score).
    // score = distance-from-bot + 15*nearbyHostiles → prefers isolated mobs, walks
    // a little further to dodge a pack center. Filters: alive / valid-hostile /
    // untapped-by-others / not combat-ignored. LOS only enforced when requireLos.
    auto pickBest = [&](std::list<Creature*> const& list, float maxDist, bool requireLos) -> Unit*
    {
        float bestScore = 99999.0f;
        Creature* best = nullptr;
        for (Creature* c : list)
        {
            if (!c->IsAlive())
                continue;
            if (!IsValidHostileTarget(c))
                continue;
            if (IsCombatIgnored(c->GetGUIDLow()))
                continue;
            if (c->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED) && !c->IsTappedBy(me))
                continue;
            float d = me->GetDistance(c);
            if (d > maxDist)
                continue;
            if (requireLos && !me->IsWithinLOSInMap(c))
                continue;

            float score = d + const_cast<AiBotAI*>(this)->CountNearbyHostiles(c, 15.0f) * AGGRO_PENALTY;
            if (score < bestScore)
            {
                bestScore = score;
                best = c;
            }
        }
        return best;
    };

    // ── Priority 2a: OBJECTIVE grind — bot-centric escalating rescan ──
    // After each kill the search re-anchors on the bot and steps the ring out
    // 50 → 100 → 150 → 200 until a valid quest mob is found, then stops. There is
    // NO fixed-center filter (the ring radius IS the whole allowance) and LOS is
    // dropped (we path to the spawn — we don't need to see it). First ring with a
    // hit wins; all four dry → fall through to filler/patrol. Gated to entry!=0 so
    // the kill-anything grind below keeps its tight 50y leash (no drift, Issue 4).
    if (m_currentTask.creatureEntry != 0)
    {
        static float const kRungs[] = { 50.0f, 100.0f, 150.0f, 200.0f };
        for (float rung : kRungs)
        {
            std::list<Creature*> creatures;
            const_cast<Player*>(me)->GetCreatureListWithEntryInGrid(
                creatures, m_currentTask.creatureEntry, rung);

            if (Unit* pick = pickBest(creatures, rung, /*requireLos*/ false))
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-GRIND] %s: objective rescan hit entry=%u at %.0fyd (ring=%.0f)",
                    me->GetName(), m_currentTask.creatureEntry, me->GetDistance(pick), rung);
                return pick;
            }
        }

        // Quest mob dry within 200y → opportunistic filler within 50y. Keeps the
        // bot killing (XP/loot) AND the filler KILL resets the C# 120s objective
        // deadline (confirmed-working — do NOT drop). Kill-counting is unaffected:
        // UpdateAI only credits the objective when victim entry == creatureEntry.
        std::list<Unit*> fillers;
        MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck u_check(me, me, 50.0f);
        MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(fillers, u_check);
        Cell::VisitAllObjects(me, searcher, 50.0f);

        std::list<Creature*> fillerCreatures;
        for (Unit* u : fillers)
            if (u->IsCreature())
                fillerCreatures.push_back(static_cast<Creature*>(u));

        if (Unit* pick = pickBest(fillerCreatures, 50.0f, /*requireLos*/ true))
            return pick;

        return nullptr;   // 200y dry + no filler → caller falls back to DoGrindPatrol
    }

    // ── Priority 2b: INDEFINITE grind — NEAREST LEVEL-APPROPRIATE XP MOB (bot-centric) ──
    // The directive, literally: nearest mob that gives XP → kill it → rescan. No density
    // scoring, no center leash. Skip critters (type 8) and grey (no XP) so the bot never
    // farms chickens/sheep; skip > L+HIGH so an L7 doesn't dive a red (it instead finds
    // nothing valid → no real kills → the C# no-progress breaker relocates/hearths it).
    uint32 const myLevel   = me->GetLevel();
    uint32 const grayLevel = AiBotGrayLevel(myLevel);

    std::list<Unit*> targets;
    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck u_check(me, me, AIBOT_GRIND_SCAN_YARDS);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(targets, u_check);
    Cell::VisitAllObjects(me, searcher, AIBOT_GRIND_SCAN_YARDS);

    float bestDist = 99999.0f;
    Creature* bestTarget = nullptr;
    for (Unit* pUnit : targets)
    {
        if (!pUnit->IsCreature())
            continue;
        Creature* c = static_cast<Creature*>(pUnit);
        if (!c->IsAlive())
            continue;
        if (!IsValidHostileTarget(c))
            continue;
        if (IsCombatIgnored(c->GetGUIDLow()))
            continue;
        if (c->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED) && !c->IsTappedBy(me))
            continue;

        // skip critters (Cow/Sheep/Chicken — 0 XP farm)
        if (CreatureInfo const* ci = c->GetCreatureInfo())
            if (ci->type == CREATURE_TYPE_CRITTER)
                continue;

        // skip grey (no XP) and reds (don't suicide — leave the bot to find something killable)
        uint32 const cl = c->GetLevel();
        if (cl <= grayLevel)
            continue;
        if (cl > myLevel + AIBOT_GRIND_HIGH_OFFSET)
            continue;

        if (!me->IsWithinLOSInMap(c))
            continue;

        float d = me->GetDistance(c);
        if (d < bestDist)
        {
            bestDist = d;
            bestTarget = c;
        }
    }

    return bestTarget;
}


void AiBotAI::DoGrindPatrol()
{
    if (me->IsMoving() || me->IsInCombat())
        return;
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
        return;
    if (m_wanderTimer > 0)
        return;

    m_wanderTimer = urand(3000, 6000);  // faster than idle wander

    // Random point within grind radius from area center
    float angle = (float)(urand(0, 628)) / 100.0f;  // 0 to ~2*PI
    float dist  = (float)(urand(0, (uint32)(m_currentTask.radius * 100))) / 100.0f;
    float x = m_currentTask.x + dist * cos(angle);
    float y = m_currentTask.y + dist * sin(angle);
    float z = m_currentTask.z;

    me->GetRandomPoint(x, y, z, 5.0f, x, y, z);  // snap to valid terrain
    MovePointRun(AIBOT_POINT_GRIND_PATROL, x, y, z);
}

// ============================================================
// §4: ScanApproachTarget — engage ANY valid in-log quest mob on the way
//     (COMPLETE METHOD REPLACEMENT — scatter "engage any valid mob" fix)
//
// The enriched MOVE_TO carries ONE creatureEntry (the dispatched objective, or the
// C#-resolved item-drop source). The OLD scan keyed ONLY to that entry AND required
// LOS, so the bot ran right past mobs valid for a DIFFERENT in-log quest — or the SAME
// entry in a closer, live cluster it couldn't quite see — to march to the dispatched
// scatter coord. If another bot had already farmed that coord out, it dead-ended into
// GRIND_BLOCKED / "un-continuable" beside perfectly good targets.
//
// Now it engages the nearest live mob valid for ANY unmet kill objective we hold. The
// valid-kill UNION is built straight from the server-authoritative QuestStatusMap — no
// new wire field, no new task member:
//   • for every INCOMPLETE quest, each ReqCreatureOrGOId slot that is a CREATURE (>0)
//     and still SHORT of its required count (a met slot's mob gives no quest credit), and
//   • the dispatched creatureEntry itself — which is the ONLY known kill creature for a
//     pure item-drop quest (the core quest row carries the item, not the creature; C#
//     resolved the drop source into creatureEntry).
//
// One bot-centric unfriendly sweep (not a per-entry grid query), filtered by entry ∈
// union. LOS is DROPPED to match the objective rescan in SelectGrindTarget — we PATH to
// the mob, we don't need to see it. The tap gate (UNIT_DYNFLAG_TAPPED && !IsTappedBy) and
// the combat-ignore set are honored exactly as the kill-credit path and the picker do.
//
// NOTE (kill-counting): the dispatched leg's killCount still only advances on
// creatureEntry (UpdateAI). A mob engaged here for a DIFFERENT entry is credited to its
// own quest SERVER-side (shared kill credit), seen on the next QUERY_QUEST_STATUS — the
// bot makes real batch progress instead of stranding on a dead coord. Per-leg counting of
// cross-entry kills is the follow-on that needs a first-class task-side union (header).
// ============================================================
Unit* AiBotAI::ScanApproachTarget()
{
    if (m_currentTask.creatureEntry == 0)
        return nullptr;

    // ── Build the valid-kill union from our own quest log ──
    std::set<uint32> validEntries;
    validEntries.insert(m_currentTask.creatureEntry);   // always the dispatched objective / item-drop source

    const auto& questMap = me->GetQuestStatusMap();
    for (const auto& pair : questMap)
    {
        const auto& qData = pair.second;
        if (qData.m_status != QUEST_STATUS_INCOMPLETE)
            continue;   // COMPLETE = objectives met (no kills owed); NONE/UNAVAILABLE = not held
        Quest const* pQuest = sObjectMgr.GetQuestTemplate(pair.first);
        if (!pQuest)
            continue;
        for (int j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
        {
            int32 reqId = pQuest->ReqCreatureOrGOId[j];
            if (reqId <= 0)
                continue;   // 0 = empty slot; <0 = gameobject objective (not a kill)
            if (qData.m_creatureOrGOcount[j] >= pQuest->ReqCreatureOrGOCount[j])
                continue;   // this slot is already satisfied — its mob yields no quest credit
            validEntries.insert((uint32)reqId);
        }
    }

    // ── Bot-centric sweep for the nearest valid mob (no center-radius filter, no LOS) ──
    float searchRadius = (m_currentTask.radius > 10.0f) ? std::min(m_currentTask.radius, 60.0f) : 60.0f;

    std::list<Unit*> nearby;
    MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck u_check(me, me, searchRadius);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(nearby, u_check);
    Cell::VisitAllObjects(me, searcher, searchRadius);

    float bestDist = searchRadius;
    Unit* best = nullptr;
    for (Unit* u : nearby)
    {
        if (!u->IsCreature())
            continue;
        Creature* c = static_cast<Creature*>(u);
        if (!c->IsAlive())
            continue;
        if (validEntries.find(c->GetEntry()) == validEntries.end())
            continue;   // not valid for any unmet objective we hold
        if (!IsValidHostileTarget(c))
            continue;
        if (IsCombatIgnored(c->GetGUIDLow()))
            continue;
        if (c->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED) && !c->IsTappedBy(me))
            continue;
        // NO center-radius filter, NO LOS — scan around the bot, path to it (§4.2).
        float d = me->GetDistance(c);
        if (d < bestDist)
        {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

// ============================================================
// §4: ConvertMoveToGrindInPlace — turn the enriched MOVE_TO into a GRIND, here
//
// Re-centering the grind on the bot's CURRENT position is the whole "we don't
// need the exact center" enrichment: SelectGrindTarget's center-radius filter
// (filter #3) now passes for the local cluster instead of rejecting it against
// the deep coord. creatureEntry / killGoal / killCount are left intact — the grind
// counts kills (m_lastVictimEntry == creatureEntry) and emits TASK_COMPLETE at
// kill_count exactly as a normal grind, which the C# merged step reads as
// "objective done". Issues no movement → reentrancy-safe from a motion callback.
// ============================================================
void AiBotAI::ConvertMoveToGrindInPlace()
{
    m_currentTask.type = TASK_GRIND;
    m_currentTask.x = me->GetPositionX();
    m_currentTask.y = me->GetPositionY();
    m_currentTask.z = me->GetPositionZ();
    if (m_currentTask.radius < 10.0f)
        m_currentTask.radius = 40.0f;
    ClearStoredPath();

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-PATH] %s: objective MOVE_TO -> GRIND in place (entry=%u goal=%d r=%.0f) at (%.1f, %.1f, %.1f)",
        me->GetName(), m_currentTask.creatureEntry, m_currentTask.killGoal,
        m_currentTask.radius, m_currentTask.x, m_currentTask.y, m_currentTask.z);
}