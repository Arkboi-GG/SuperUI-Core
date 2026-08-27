/*
 * AiBotAIRaidPlan.cpp — the raid plan ACTING on the bot (PLAN_19 M-D, v1).
 *
 * Consumes the m_raidPlan slice BridgeHandleLoadRaidPlan adopted (M-C) and turns
 * it into behaviour, on a 500ms sub-tick from UpdateAI (the rotation sub-tick
 * pattern):
 *
 *   FORMATION — the bot keeps a computed station on the fight's boss via the
 *     motion master's own MoveChase(dist, angle): the MT on her nose at reach,
 *     melee fanned across the safe flank band (SuiFormationLaw, the client-parity
 *     math), healers on the mid ring, ranged at standoff. Slot index/count/side
 *     and the MT flag arrive pre-resolved on the wire (the web push knows the
 *     whole roster; one bot only knows itself). v1 uses CONSERVATIVE DEFAULT
 *     ARCS (45° front half, 60° rear half, 15 yd cone range) — deriving the real
 *     arcs from spell_cone for the specific boss is a later refinement and is
 *     labelled here so nobody mistakes the default for boss truth.
 *
 *   MAINTAINED AURAS — "keep Fear Ward on the tank": the ward target is the
 *     boss's REAL current victim (server threat truth — better than the client's
 *     ordinal guess), cast through the same CanTryToCastSpell/DoCastSpell path
 *     the class AI uses, with the rule's cooldown tracked per bot. KNOWN v1
 *     LIMIT: two same-class bots can race the same refresh inside one sub-tick
 *     window (a wasted cooldown, never a wrong state); the cross-bot chain
 *     ordering lives with the coordinator later.
 *
 *   ADD DUTY — RaidPlanPreferredAdd feeds the EncounterPlay doctrine's
 *     MaintainTarget (AiBotDoctrineEncounter.cpp): a bot whose plan puts AnyAdd
 *     first in any phase order commits to the nearest live non-boss hostile
 *     whenever one is in the fight. Phase GATING is a later refinement (the core
 *     does not yet track encounter phase); v1 add duty is fight-wide, and that
 *     is exactly the testable "spare warriors peel the whelps" behaviour.
 *
 * Movement precedence honoured: the sub-tick only ever (re)issues over CHASE /
 * FOLLOW / IDLE motion — a bridge MOVE_TO (point motion), a possession, or any
 * scripted movement outranks the formation by simply not being touched.
 *
 * Line endings: LF (C++ repo convention).
 */

#include "AiBotAIMain.h"
#include "AiBotCircuit.h"   // [CIRCUIT] probe macros (CIRCUIT_BOARD.md)
#include "RaidPlanLaw.h"

#include "Player.h"
#include "Group.h"
#include "SpellMgr.h"
#include "MotionMaster.h"
#include "Timer.h"
#include "Log.h"

#include <cmath>

// v1 default standing-threat picture (see file header): a 90° frontal cone and a
// 120° rear cone at 15 yd — the Onyxia shape, and a safe conservative default for
// any boss that swings and tail-sweeps.
static constexpr float RAIDPLAN_DEFAULT_FRONT_HALF_RAD = 0.7853982f;
static constexpr float RAIDPLAN_DEFAULT_REAR_HALF_RAD  = 1.0471976f;
static constexpr float RAIDPLAN_DEFAULT_CONE_RANGE_YD  = 15.0f;

// ============================================================
// The fight's boss: the hostile this bot is in combat with that has the largest
// health pool. Whelps beside Onyxia never out-pool her; a trash pack without a
// boss simply nominates its biggest member, which degrades to sane behaviour.
// ============================================================
Unit* AiBotAI::RaidPlanFindBoss()
{
    if (!me || !me->IsInWorld())
    {   // cb:fold probed on next line
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-raid: boss scan skipped, not in world");
        return nullptr;
    }

    Unit* best = nullptr;
    uint32 bestHealth = 0;
    HostileReference* ref = me->GetHostileRefManager().getFirst();
    while (ref)
    {
        if (Unit* hostile = ref->getSourceUnit()) // cb:fold hostile deref, candidate probed inside
        {   // cb:fold hostile deref, candidate probed inside
            if (hostile->IsAlive() && IsValidHostileTarget(hostile) &&
                me->IsWithinDist(hostile, VISIBILITY_DISTANCE_NORMAL) &&
                hostile->GetMaxHealth() > bestHealth)
            {   // cb:fold probed on next line
                CB_HITV(me->GetGUIDLow(), "cpp-raid: boss candidate bigger pool", hostile->GetMaxHealth());
                bestHealth = hostile->GetMaxHealth();
                best = hostile;
            }
        }
        ref = ref->next();
    }
    return best;
}

// ============================================================
// Add duty: any phase order in this bot's slice that puts AnyAdd (0) first.
// Phase-independent in v1 — see the file header.
// ============================================================
bool AiBotAI::RaidPlanOnAddDuty() const
{
    if (!m_hasRaidPlan)
        return false; // cb:fold no raid plan loaded, duty probed at grant
    for (auto const& pt : m_raidPlan.phaseTargets)
        if (!pt.order.empty() && pt.order[0] == 0)
        {   // cb:fold probed on next line
            CB_HIT(me->GetGUIDLow(), "cpp-raid: add duty found in phase order");
            return true;
        }
    return false;
}

// ============================================================
// The add this bot should be on: nearest live non-boss hostile in the fight.
// nullptr = no duty or no add — the EncounterPlay doctrine then defers inward.
// ============================================================
Unit* AiBotAI::RaidPlanPreferredAdd()
{
    if (!RaidPlanOnAddDuty() || !me || !me->IsInCombat())
    {   // cb:fold probed on next line
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-raid: no add duty or out of combat");
        return nullptr;
    }

    Unit* boss = RaidPlanFindBoss();
    Unit* best = nullptr;
    float bestDistance = 0.f;
    HostileReference* ref = me->GetHostileRefManager().getFirst();
    while (ref)
    {
        if (Unit* hostile = ref->getSourceUnit()) // cb:fold hostile deref, eligibility probed inside
        {   // cb:fold hostile deref, eligibility probed inside
            if (hostile != boss && hostile->IsAlive() && IsValidHostileTarget(hostile) &&
                me->IsWithinDist(hostile, VISIBILITY_DISTANCE_NORMAL))
            {   // cb:fold probed on next line
                CB_HITV(me->GetGUIDLow(), "cpp-raid: add candidate eligible", hostile->GetEntry());
                float distance = me->GetDistance(hostile);
                if (!best || distance < bestDistance)
                {   // cb:fold probed on next line
                    CB_HITV(me->GetGUIDLow(), "cpp-raid: add nearest so far", distance);
                    best = hostile;
                    bestDistance = distance;
                }
            }
        }
        ref = ref->next();
    }
    return best;
}

// ============================================================
// The 500ms act cadence (called from UpdateAI's sub-tick).
// ============================================================
void AiBotAI::UpdateRaidPlanTick()
{
    if (!m_hasRaidPlan || !me || !me->IsInWorld() || !me->IsAlive())
        return; // cb:fold plan tick idle, no plan or bot gone (fires every sub-tick fleet-wide)

    RaidPlanMaintainAuras();

    if (m_raidPlan.doctrine.deriveFormation && me->IsInCombat())
    {   // cb:fold probed on next line
        CB_HIT(me->GetGUIDLow(), "cpp-raid: formation sub-tick");
        RaidPlanFormation();
    }
}

// ============================================================
// Maintained auras — the Fear Ward law against server truth.
// ============================================================
void AiBotAI::RaidPlanMaintainAuras()
{
    if (m_raidPlan.doctrine.maintainAuras.empty())
        return; // cb:fold no maintained auras in this plan (fires every sub-tick)
    if (me->IsNonMeleeSpellCasted(false, false, true))
    {   // cb:fold probed on next line
        CB_HIT(me->GetGUIDLow(), "cpp-raid: aura tick hold, casting");
        return;
    }

    // The ward target is the REAL tank: whoever the boss is actually on, when
    // that is a player in this bot's own group. Out of combat there is no boss
    // victim, so v1 holds its wards for the pull — an honest gap.
    Player* ward = nullptr;
    if (Unit* boss = me->IsInCombat() ? RaidPlanFindBoss() : nullptr) // cb:fold ward deref chain, resolution probed below
        if (Unit* victim = boss->GetVictim()) // cb:fold ward deref chain, resolution probed below
            if (Player* player = victim->ToPlayer()) // cb:fold ward deref chain, resolution probed below
                if (me->GetGroup() && player->GetGroup() == me->GetGroup()) // cb:fold ward deref chain, resolution probed below
                    ward = player; // cb:fold ward deref chain, resolution probed below
    if (!ward)
        return; // cb:fold no ward target yet, resolution probed below (out of combat holds wards)

    CB_HIT(me->GetGUIDLow(), "cpp-raid: ward target resolved");
    uint32 const now = WorldTimer::getMSTime();
    for (auto const& rule : m_raidPlan.doctrine.maintainAuras)
    {
        if (me->GetClass() != rule.casterClassId)
            continue; // cb:fold aura rule for another class, cast probed below
        if (!me->HasSpell(rule.spellId))
        {   // cb:fold probed on next line
            CB_HITV(me->GetGUIDLow(), "cpp-raid: aura spell not known", rule.spellId);
            continue;
        }
        if (ward->HasAura(rule.spellId, EFFECT_INDEX_0))
        {   // cb:fold probed on next line
            CB_HITV(me->GetGUIDLow(), "cpp-raid: aura already up on ward", rule.spellId);
            continue;   // one is up: the chain's whole point is not doubling
        }

        auto ready = m_raidPlanAuraReady.find(rule.spellId);
        if (ready != m_raidPlanAuraReady.end() && ready->second > now)
        {   // cb:fold probed on next line
            CB_HITV(me->GetGUIDLow(), "cpp-raid: aura on personal cooldown", rule.spellId);
            continue;   // my cast of it is on cooldown — another chain link's turn
        }

        SpellEntry const* pSpell = sSpellMgr.GetSpellEntry(rule.spellId);
        if (!pSpell || !CanTryToCastSpell(ward, pSpell))
        {   // cb:fold probed on next line
            CB_HITV(me->GetGUIDLow(), "cpp-raid: aura cast not possible", rule.spellId);
            continue;
        }
        if (DoCastSpell(ward, pSpell) == SPELL_CAST_OK)
        {   // cb:fold probed on next line
            CB_HITV(me->GetGUIDLow(), "cpp-raid: aura cast ok on ward", rule.spellId);
            m_raidPlanAuraReady[rule.spellId] = now + (uint32)(rule.cooldownMs > 0 ? rule.cooldownMs : 0);
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-RAIDPLAN] %s: keeps aura %u on %s (plan '%s')",
                me->GetName(), rule.spellId, ward->GetName(), m_raidPlan.name.c_str());
            break;   // one maintenance cast per tick
        }
    }
}

// ============================================================
// Formation — the computed station, expressed through the motion master's own
// chase generator so pathing, following and facing stay core-owned.
// ============================================================
void AiBotAI::RaidPlanFormation()
{
    Unit* boss = RaidPlanFindBoss();
    if (!boss)
    {   // cb:fold probed on next line
        CB_HIT(me->GetGUIDLow(), "cpp-raid: formation hold, no boss");
        return;
    }

    // A bot fighting its assigned ADD is where it should be: the normal engage
    // chase owns it, and the formation must not drag it back to the boss.
    Unit* victim = me->GetVictim();
    if (victim && victim != boss)
    {   // cb:fold probed on next line
        CB_HIT(me->GetGUIDLow(), "cpp-raid: formation hold, fighting add");
        return;
    }

    // Precedence: only ever (re)issue over chase/follow/idle motion. Point motion
    // (a bridge MOVE_TO, a scripted run) and everything else outranks the station.
    MovementGeneratorType motion = me->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if (motion != CHASE_MOTION_TYPE && motion != FOLLOW_MOTION_TYPE && motion != IDLE_MOTION_TYPE)
    {   // cb:fold probed on next line
        CB_HITV(me->GetGUIDLow(), "cpp-raid: formation hold, motion outranks", motion);
        return;
    }
    if (me->IsNonMeleeSpellCasted(false, false, true))
    {   // cb:fold probed on next line
        CB_HIT(me->GetGUIDLow(), "cpp-raid: formation hold, casting");
        return;
    }

    float from, to;
    SuiFormationLaw::SafeBand(RAIDPLAN_DEFAULT_FRONT_HALF_RAD,
        RAIDPLAN_DEFAULT_REAR_HALF_RAD, from, to);

    // side: wire 1 = Left/Group 1 (+angle = her left), 3 = Right/Group 2.
    int const sideSign = m_raidPlan.side == 3 ? -1 : 1;
    bool const mainTank = m_raidPlan.mainTank != 0 ||
        (m_raidPlan.job == 1 && boss->GetVictim() == me);

    float angle, dist;
    if (mainTank)
    {   // cb:fold probed on next line
        CB_HIT(me->GetGUIDLow(), "cpp-raid: station main tank nose");
        angle = 0.f;    // her nose: aiming her is his job
        dist = 1.0f;
    }
    else
    {   // cb:fold probed on next line
        CB_HITV(me->GetGUIDLow(), "cpp-raid: station slot fan", m_raidPlan.slotIndex);
        angle = (float)sideSign * SuiFormationLaw::SlotAngle(
            from, to, m_raidPlan.slotIndex, m_raidPlan.slotCount);
        float const reach = 5.5f;   // representative; the chase dist is what binds
        switch (m_raidPlan.job)
        {
            case 1:   // spare tank: with the melee at reach, ready to peel // cb:fold shares the melee reach arm below
            case 3:   // melee // cb:fold probed on next line
                CB_HIT(me->GetGUIDLow(), "cpp-raid: station melee reach");
                dist = 1.0f;
                break;
            case 2:   // healer: the mid ring // cb:fold probed on next line
                CB_HIT(me->GetGUIDLow(), "cpp-raid: station healer ring");
                dist = SuiFormationLaw::HealerRadius(RAIDPLAN_DEFAULT_CONE_RANGE_YD, reach);
                break;
            default:  // ranged and unknown: standoff // cb:fold probed on next line
                CB_HIT(me->GetGUIDLow(), "cpp-raid: station ranged standoff");
                dist = SuiFormationLaw::RangedRadius(RAIDPLAN_DEFAULT_CONE_RANGE_YD, reach);
                break;
        }
    }

    // Re-issue only on real drift — churning the motion master every sub-tick
    // would fight its own arrival damping.
    if (m_raidPlanChaseGuid == boss->GetObjectGuid() &&
        motion == CHASE_MOTION_TYPE &&
        std::fabs(angle - m_raidPlanChaseAngle) < 0.12f &&
        std::fabs(dist - m_raidPlanChaseDist) < 0.5f)
        return; // cb:fold station unchanged within damping, issue probed below

    CB_HITV(me->GetGUIDLow(), "cpp-raid: station issued", dist);
    me->GetMotionMaster()->MoveChase(boss, dist, angle);
    m_raidPlanChaseGuid = boss->GetObjectGuid();
    m_raidPlanChaseAngle = angle;
    m_raidPlanChaseDist = dist;
    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
        "[AIBOT-RAIDPLAN] %s: formation station on %s — angle %.2f dist %.1f (slot %d/%d side %+d%s)",
        me->GetName(), boss->GetName(), angle, dist,
        m_raidPlan.slotIndex, m_raidPlan.slotCount, sideSign, mainTank ? ", MT" : "");
}
