/*
 * AiBotAICombat.cpp — combat behaviour for the autonomous AI bot.
 *
 * Split from the monolithic AiBotAI.cpp. THIS TU holds the combat domain:
 *   - mount / eat-drink / aggro-distance utility
 *   - the combat-ignore set + the [TEAMPLAY] assist-target seam (IsValidAssistTarget)
 *   - the stalemate breaker and overpull-retreat discipline
 *   - AttackStart / SelectAttackTarget / CheckForUnreachableTarget
 *   - the UpdateInCombatAI / UpdateOutOfCombatAI class dispatchers
 *   - all 18 per-class combat methods (verbatim from BattleBotAI)
 *
 * All members of AiBotAI, so they link across the sibling TUs transparently.
 * Cross-TU members called from here (StopMoving / ReGroundZ / FindNavBoundaryNear from
 * Movement, BridgeSendEvent from Bridge, CountNearbyHostiles from Grind) are defined in
 * those siblings.
 */

#include "AiBotAIMain.h"
#include "AiBotAITeamPlay.h"   // [TEAMPLAY] ResolveCombatTarget — the group focus-fire resolver
#include "AiBotCircuit.h"      // [CIRCUIT] probe macros (CIRCUIT_BOARD.md)
#include "Player.h"
#include <cstring>
#include <cstdio>
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

// BG flag auras (referenced in the combat methods below — never active in open world).
// File-local: these formerly lived in the AiBotSpells enum, but that enum moved to the shared
// header during the split, and BattleBotAI.h also defines these two (enum FlagSpellsWS) — so a
// TU including both headers (PlayerBotMgr.cpp) would conflict. This TU does NOT include
// BattleBotAI.h, so a file-local definition is safe and keeps the combat bodies byte-for-byte.
enum AiBotCombatFlagAuras
{
    AURA_WARSONG_FLAG    = 23333,
    AURA_SILVERWING_FLAG = 23335,
};

// ============================================================
// UTILITY METHODS (adapted from BattleBotAI — BG code removed)
// ============================================================



uint32 AiBotAI::GetMountSpellId() const
{
    if (me->GetLevel() >= 60)
    {   // cb:fold hot per-update detail
        if (me->GetClass() == CLASS_PALADIN)
            return AB_SPELL_MOUNT_60_PALADIN;   // cb:fold hot per-update detail
        if (me->GetClass() == CLASS_WARLOCK)
            return AB_SPELL_MOUNT_60_WARLOCK;   // cb:fold hot per-update detail

        switch (me->GetRace())
        {
            case RACE_HUMAN:    return AB_SPELL_MOUNT_60_HUMAN;   // cb:fold hot per-update detail
            case RACE_NIGHTELF: return AB_SPELL_MOUNT_60_NELF;   // cb:fold hot per-update detail
            case RACE_DWARF:    return AB_SPELL_MOUNT_60_DWARF;   // cb:fold hot per-update detail
            case RACE_GNOME:    return AB_SPELL_MOUNT_60_GNOME;   // cb:fold hot per-update detail
            case RACE_TROLL:    return AB_SPELL_MOUNT_60_TROLL;   // cb:fold hot per-update detail
            case RACE_ORC:      return AB_SPELL_MOUNT_60_ORC;   // cb:fold hot per-update detail
            case RACE_TAUREN:   return AB_SPELL_MOUNT_60_TAUREN;   // cb:fold hot per-update detail
            case RACE_UNDEAD:   return AB_SPELL_MOUNT_60_UNDEAD;   // cb:fold hot per-update detail
        }
    }
    else if (me->GetLevel() >= 40)
    {   // cb:fold hot per-update detail
        if (me->GetClass() == CLASS_PALADIN)
            return AB_SPELL_MOUNT_40_PALADIN;   // cb:fold hot per-update detail
        if (me->GetClass() == CLASS_WARLOCK)
            return AB_SPELL_MOUNT_40_WARLOCK;   // cb:fold hot per-update detail

        switch (me->GetRace())
        {
            case RACE_HUMAN:    return AB_SPELL_MOUNT_40_HUMAN;   // cb:fold hot per-update detail
            case RACE_NIGHTELF: return AB_SPELL_MOUNT_40_NELF;   // cb:fold hot per-update detail
            case RACE_DWARF:    return AB_SPELL_MOUNT_40_DWARF;   // cb:fold hot per-update detail
            case RACE_GNOME:    return AB_SPELL_MOUNT_40_GNOME;   // cb:fold hot per-update detail
            case RACE_TROLL:    return AB_SPELL_MOUNT_40_TROLL;   // cb:fold hot per-update detail
            case RACE_ORC:      return AB_SPELL_MOUNT_40_ORC;   // cb:fold hot per-update detail
            case RACE_TAUREN:   return AB_SPELL_MOUNT_40_TAUREN;   // cb:fold hot per-update detail
            case RACE_UNDEAD:   return AB_SPELL_MOUNT_40_UNDEAD;   // cb:fold hot per-update detail
        }
    }

    return 0;
}

bool AiBotAI::UseMount()
{
    if (me->IsMounted())
        return false;   // cb:fold hot per-update detail

    if (me->IsMoving())
        return false;   // cb:fold hot per-update detail

    if (me->GetDisplayId() != me->GetNativeDisplayId())
        return false;   // cb:fold hot per-update detail

    if (me->GetClass() == CLASS_ROGUE)
        return false;   // cb:fold hot per-update detail

    if (me->HasAura(SPELL_AURA_MOD_STEALTH))
        return false;   // cb:fold hot per-update detail

    uint32 spellId = GetMountSpellId();
    if (!spellId)
        return false;   // cb:fold hot per-update detail

    if (me->CastSpell(me, spellId, false) == SPELL_CAST_OK)
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: mounted up", spellId);
        return true;
    }

    return false;
}

bool AiBotAI::DrinkAndEat()
{
    if (m_isBuffing)
        return false;   // cb:fold hot per-update detail

    if (me->IsMounted())
        return false;   // cb:fold hot per-update detail

    if (me->GetVictim())
        return false;   // cb:fold hot per-update detail

    bool const needToEat = me->GetHealthPercent() < 100.0f;
    bool const needToDrink = (me->GetPowerType() == POWER_MANA) && (me->GetPowerPercent(POWER_MANA) < 100.0f);

    if (!needToEat && !needToDrink)
        return false;   // cb:fold hot per-update detail

    bool const isEating = me->HasAura(AB_SPELL_FOOD);
    bool const isDrinking = me->HasAura(AB_SPELL_DRINK);

    if (!isEating && needToEat)
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: eating, hp below full", me->GetHealthPercent());
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {   // cb:fold hot per-update detail
            StopMoving();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(AB_SPELL_FOOD))
        {   // cb:fold hot per-update detail
            me->CastSpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(*pSpellEntry);
        }
        return true;
    }

    if (!isDrinking && needToDrink)
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: drinking, mana below full", me->GetPowerPercent(POWER_MANA));
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
        {   // cb:fold hot per-update detail
            StopMoving();
        }
        if (SpellEntry const* pSpellEntry = sSpellMgr.GetSpellEntry(AB_SPELL_DRINK))
        {   // cb:fold hot per-update detail
            me->CastSpell(me, pSpellEntry, true);
            me->RemoveSpellCooldown(*pSpellEntry);
        }
        return true;
    }

    return needToEat || needToDrink;
}

float AiBotAI::GetMaxAggroDistanceForMap() const
{
    return 50.0f;
}


bool AiBotAI::IsCombatIgnored(uint32 guidLow) const
{
    return guidLow != 0 && m_combatIgnore.find(guidLow) != m_combatIgnore.end();
}

// [TEAMPLAY] Public seam for TeamPlay::ResolveCombatTarget — "is the anchor's victim something
// I may legally focus?" Wraps the (AI-internal) IsValidHostileTarget + an alive check, so the
// free-function resolver never reaches into protected combat internals.
bool AiBotAI::IsValidAssistTarget(Unit* pTarget) const
{
    return pTarget && pTarget->IsAlive() && IsValidHostileTarget(pTarget);
}

// ============================================================
// Combat stalemate breaker — in-combat with a victim neither side can damage
// (bot stranded on a navmesh seam / mob unreachable across geometry). Respects
// real fights: only no-damage-either-way for AIBOT_STALEMATE_NUDGE_MS trips it.
//
// Stage 1 nudges (short-ignore the mob so we don't re-chase mid-hop).
//
// Stage 2 (CHANGED 2026-06-22): the old version did CombatStop(true)+StopMoving
// IN PLACE on the premise "the unreachable mob then evades." That premise is false
// when the mob stays in aggro range: dropping OUR combat doesn't make an adjacent
// mob evade, so it re-aggros next tick and the lock reforms forever (Hu's metronomic
// combat_stalemate loop, hp=100% throughout). The 60s guid-ignore only stops US from
// re-SELECTING it — it can't stop the mob re-engaging US. So Stage 2 now EXTRACTS the
// bot: first a real ~30yd flee AWAY from the stuck mob (the proven HandleOverpullRetreat
// pattern) so the mob loses contact + leashes; after AIBOT_STALEMATE_MAX_DISENGAGES
// flees that still haven't broken it (bot on a no-navmesh tile — the water-spawn strand),
// a hard NearTeleportTo to a known-good anchor (nav-seam outer point, else spawn).
//
// NO-VICTIM FIX (2026-07-03): me->GetVictim()==nullptr with the combat flag still set
// (the ordinary ~5s tag-linger right after a kill, or any other transient gap) used to
// fall into the SAME no-damage-either-way path as a genuine deadlock — botHp unchanged
// (nobody's hitting us) read as "no damage moved," so a bot that just killed its last
// target and has nothin0g left to fight would accumulate stalemate time and nudge
// against guid 0 (confirmed live: Piv, 2026-07-03 — both observed nudges logged
// "vs guid 0"). There is nothing to be deadlocked AGAINST with no victim, so bail
// before the accumulator. This also stops those non-problems from burning the
// Stage-1/Stage-2 escalation budget a genuine stuck-mob deadlock will want.
//
// Returns true when it acted this tick (caller must skip combat AI / return).
// ============================================================
bool AiBotAI::HandleCombatStalemate()
{
    // [PULL] A proactive pull-and-retreat is deliberately no-damage while we close in and drag the
    // mob to open ground — that is NOT a deadlock. Don't accumulate stalemate time or nudge during
    // it (a nudge would fight the retreat). Keep m_lastHealth fresh so the detector compares against
    // current HP when the real fight resumes. HandlePullRetreat owns the pull sequence. (FINDING_005)
    if (m_pullActive)
    {   // cb:fold hot per-update detail
        m_stalemateMs = 0;
        m_lastHealth = me->GetHealth();
        return false;
    }

    if (!me->IsInCombat())
    {   // cb:fold hot per-update detail
        // Out of combat. If we're mid-disengage-hop, let it finish before wiping state —
        // a Stage-2 flee that briefly drops combat must NOT reset m_stalemateDisengages,
        // or a persistent re-aggro would flee 3× / reset / flee 3× forever and never reach
        // the teleport backstop. Once the hop is done and we're still out of combat, the
        // lock is genuinely broken → full reset.
        if (m_stalemateHoldMs > AIBOT_UPDATE_INTERVAL)
        {   // cb:fold hot per-update detail
            m_stalemateHoldMs -= AIBOT_UPDATE_INTERVAL;
            return true;
        }
        m_stalemateMs = 0;
        m_stalemateHoldMs = 0;
        m_stalemateNudges = 0;
        m_stalemateDisengages = 0;
        m_stalemateVictim.Clear();
        return false;
    }

    // Mid-nudge / mid-flee hold (in combat): let the hop run; don't let the combat AI
    // re-chase this tick.
    if (m_stalemateHoldMs > 0)
    {   // cb:fold hot per-update detail
        m_stalemateHoldMs = (m_stalemateHoldMs > AIBOT_UPDATE_INTERVAL)
                          ? m_stalemateHoldMs - AIBOT_UPDATE_INTERVAL : 0;
        return true;
    }

    Unit* pVictim = me->GetVictim();

    // NO-VICTIM FIX — see header comment. Nothing to be deadlocked against; don't
    // accumulate, don't nudge. m_lastHealth still tracks so a real fight starting next
    // tick compares against fresh HP instead of a stale reading from before the gap.
    if (!pVictim)
    {   // cb:fold hot per-update detail
        m_lastHealth = me->GetHealth();
        return false;
    }

    uint32 botHp = me->GetHealth();
    uint32 vicHp = pVictim->GetHealth();

    bool damageMoved = (botHp != m_lastHealth) ||
                       (m_lastVictimHealth != 0 && vicHp != m_lastVictimHealth);
    m_lastHealth = botHp;
    m_lastVictimHealth = vicHp;

    // Damage flowing either way → genuine fight. Respect it (and clear the disengage
    // counter — a real fight means we are NOT stalemate-locked).
    if (damageMoved)
    {   // cb:fold hot per-update detail
        m_stalemateMs = 0;
        m_stalemateNudges = 0;
        m_stalemateDisengages = 0;
        m_stalemateVictim.Clear();
        return false;
    }

    // No damage either way — anchor the stalemate target once, then accumulate.
    if (m_stalemateVictim.IsEmpty())
        m_stalemateVictim = pVictim->GetObjectGuid();   // cb:fold hot per-update detail

    m_stalemateMs += AIBOT_UPDATE_INTERVAL;
    if (m_stalemateMs < AIBOT_STALEMATE_NUDGE_MS)
        return false;   // cb:fold hot per-update detail
    m_stalemateMs = 0;

    uint32 stuckGuidLow = m_stalemateVictim.GetCounter();

    // ── Stage 2: nudges exhausted → break the lock for real (flee, then teleport) ──
    if (m_stalemateNudges >= AIBOT_STALEMATE_MAX_NUDGES)
    {   // cb:fold hot per-update detail
        if (stuckGuidLow)
            m_combatIgnore[stuckGuidLow] = AIBOT_STALEMATE_IGNORE_MS;   // cb:fold hot per-update detail

        me->AttackStop();
        me->CombatStop(true);   // clear OUR refs; the flee/teleport below takes us out of range
        m_stalemateNudges = 0;

        // Resolve the stuck mob's position so we flee directly AWAY from it. Fall back to
        // the failing task dest (the stuck geometry), then to a random direction.
        float awayX = 0.0f, awayY = 0.0f;
        bool haveAway = false;
        if (Creature* pStuck = me->GetMap()->GetCreature(m_stalemateVictim))
        {   // cb:fold hot per-update detail
            awayX = pStuck->GetPositionX();
            awayY = pStuck->GetPositionY();
            haveAway = true;
        }
        else if (m_currentTask.x != 0.0f || m_currentTask.y != 0.0f)
        {   // cb:fold hot per-update detail
            awayX = m_currentTask.x;
            awayY = m_currentTask.y;
            haveAway = true;
        }

        m_stalemateVictim.Clear();

        // ── Hard escape: repeated flees haven't broken it → teleport out ──
        // (Bot is stranded on a tile the navmesh can't path off of — the water-spawn /
        // off-mesh strand. Even the flee no_path's, so only a teleport ignores the mesh.)
        if (m_stalemateDisengages >= AIBOT_STALEMATE_MAX_DISENGAGES)
        {   // cb:fold hot per-update detail
            CB_HIT(me->GetGUIDLow(), "cpp-combat: stalemate flee failed, hard teleport escape");
            m_stalemateDisengages = 0;

            float tx = me->GetPositionX(), ty = me->GetPositionY(), tz = me->GetPositionZ();
            const char* how = "current pos (no anchor)";
            if (NavBoundary const* b = FindNavBoundaryNear(tx, ty, AIBOT_BOUNDARY_SCOPE))
            {   // cb:fold hot per-update detail
                tx = b->outerX; ty = b->outerY; tz = b->outerZ; how = "nav-seam outer anchor";
            }
            else if (m_spawnMapId == me->GetMapId())
            {   // cb:fold hot per-update detail
                tx = m_spawnX; ty = m_spawnY; tz = m_spawnZ; how = "spawn";
            }

            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-STALEMATE] %s: flee did not break lock — HARD TELEPORT (%s) to (%.1f, %.1f, %.1f)",
                me->GetName(), how, tx, ty, tz);

            ReGroundZ(tx, ty, tz, "stalemate-tp");   // [GROUND]
            StopMoving();
            me->NearTeleportTo(tx, ty, tz, me->GetOrientation());

            std::string ev = "dest_x=" + std::to_string(m_currentTask.x) +
                             "|dest_y=" + std::to_string(m_currentTask.y) +
                             "|dest_z=" + std::to_string(m_currentTask.z) +
                             "|reason=combat_stalemate";
            BridgeSendEvent("MOVE_FAILED", ev.c_str());
            return true;
        }

        // ── Flee: hop a real distance AWAY from the stuck mob, then re-evaluate ──
        float fx = me->GetPositionX(), fy = me->GetPositionY(), fz = me->GetPositionZ();
        if (haveAway)
        {   // cb:fold hot per-update detail
            float ax = fx - awayX, ay = fy - awayY;
            float len = sqrtf(ax * ax + ay * ay);
            if (len > 0.5f)
            {   // cb:fold hot per-update detail
                fx += (ax / len) * AIBOT_STALEMATE_FLEE_DIST;
                fy += (ay / len) * AIBOT_STALEMATE_FLEE_DIST;
            }
        }
        me->GetRandomPoint(fx, fy, fz, AIBOT_STALEMATE_FLEE_DIST, fx, fy, fz);   // snap to valid terrain

        m_stalemateDisengages++;
        CB_HITV(me->GetGUIDLow(), "cpp-combat: stalemate disengage, fleeing stuck mob", m_stalemateDisengages);

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-STALEMATE] %s: disengage %u/%u — fleeing %.0fyd from stuck guid %u to (%.1f, %.1f)",
            me->GetName(), m_stalemateDisengages, AIBOT_STALEMATE_MAX_DISENGAGES,
            AIBOT_STALEMATE_FLEE_DIST, stuckGuidLow, fx, fy);

        StopMoving();
        MovePointRun(AIBOT_POINT_STALEMATE_NUDGE, fx, fy, fz);
        m_stalemateHoldMs = AIBOT_STALEMATE_FLEE_HOLD_MS;

        // Tell C# the leg failed so it re-plans now instead of eating the 480s deadline.
        std::string ev = "dest_x=" + std::to_string(m_currentTask.x) +
                         "|dest_y=" + std::to_string(m_currentTask.y) +
                         "|dest_z=" + std::to_string(m_currentTask.z) +
                         "|reason=combat_stalemate";
        BridgeSendEvent("MOVE_FAILED", ev.c_str());
        return true;
    }

    // ── Stage 1: nudge (short-ignore the mob so the combat AI can't re-chase the hop) ──
    m_stalemateNudges++;
    CB_HITV(me->GetGUIDLow(), "cpp-combat: stalemate nudge hop", m_stalemateNudges);
    if (stuckGuidLow)
        m_combatIgnore[stuckGuidLow] = AIBOT_STALEMATE_NUDGE_HOLD_MS + 500;   // cb:fold hot per-update detail

    me->AttackStop(false);

    float nx = me->GetPositionX();
    float ny = me->GetPositionY();
    float nz = me->GetPositionZ();
    if (m_currentTask.x != 0.0f || m_currentTask.y != 0.0f)   // hop toward the journey dest if we have one
    {   // cb:fold hot per-update detail
        float dx = m_currentTask.x - nx;
        float dy = m_currentTask.y - ny;
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 0.5f)
        {   // cb:fold hot per-update detail
            nx += (dx / len) * AIBOT_STALEMATE_NUDGE_DIST;
            ny += (dy / len) * AIBOT_STALEMATE_NUDGE_DIST;
        }
    }
    me->GetRandomPoint(nx, ny, nz, AIBOT_STALEMATE_NUDGE_DIST, nx, ny, nz);   // snap to valid terrain

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-STALEMATE] %s: nudge %u/%u vs guid %u — hopping to (%.1f, %.1f)",
        me->GetName(), m_stalemateNudges, AIBOT_STALEMATE_MAX_NUDGES, stuckGuidLow, nx, ny);

    StopMoving();
    MovePointRun(AIBOT_POINT_STALEMATE_NUDGE, nx, ny, nz);
    m_stalemateHoldMs = AIBOT_STALEMATE_NUDGE_HOLD_MS;
    return true;
}

// ============================================================
// [PULLGATE] PullReady — fight-initiation readiness (2026-07-05, solo discipline).
//
// The bot may DEFEND at any HP — this gates only the INITIATION of a new pull (the
// TASK_GRIND dispatch and the enriched-MOVE_TO approach scan in UpdateAI). Below the
// floor the caller latches m_eatRecoveryLatch and stands down; the OOC eat block then
// owns every tick until the latch releases at AIBOT_EAT_EXIT_HP/_MANA. Rage/energy
// classes gate on HP alone (rage starts empty by design; energy refills in seconds).
//
// Why 70: a pull initiated at 41-60% (where the old one-way eat gate parked the fleet)
// is a coin flip against an even-level mob and a guaranteed loss against one add. Every
// survival net downstream of this line fires AFTER the corpse; this is the first that
// fires before it.
// ============================================================
bool AiBotAI::PullReady() const
{
    if (me->GetHealthPercent() < AIBOT_PULL_MIN_HP)
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: pull gated, hp below floor", me->GetHealthPercent());
        return false;
    }

    if (me->GetPowerType() == POWER_MANA &&
        me->GetPowerPercent(POWER_MANA) < AIBOT_PULL_MIN_MANA)
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: pull gated, mana below floor", me->GetPowerPercent(POWER_MANA));
        return false;
    }

    return true;
}

// ════════════════════════════════════════════════════════════════════════════════════════
// AiBotAICombat.cpp — FUNCTION REPLACEMENT (2026-07-05)
//
// ONE function changes in this 1900-line TU: OverpullGuard.
//
// WHY: the old guard self-disabled when grouped ("let the pack absorb density") — written
// when every member pulled independently and a pre-pull cap on one bot meant nothing. Under
// the one-picker doctrine (AiBotDoctrineTeam.cpp, same date) the ANCHOR's pull is the TEAM's
// pull: followers never self-acquire, so the anchor diving a target buried in a dense cluster
// commits the whole trio — the 23-kobold Fargodeep camp dive from the 2026-07-04 death
// forensics. The anchor now respects the GROUP density cap (AIBOT_OVERPULL_GROUP, 6) before
// engaging; the in-combat HandleOverpullRetreat backstop is unchanged. Followers still hit
// this via HoldPull, harmlessly — they have no autonomous pulls left to veto.
// ════════════════════════════════════════════════════════════════════════════════════════

// [OVERPULL] Solo density cap scaled to survivability. AIBOT_OVERPULL_SOLO (3) is calibrated for
// a settled mid-level bot; a fresh L1 (~40 HP) dies to 2-3 even-con mobs long before the flee
// cycle recovers, and 3 attackers sits one BELOW the flat > cap trigger — so it never fled. Cap
// now scales with level, tightened further when already hurt. Grouped bots keep AIBOT_OVERPULL_GROUP.
uint32 AiBotAI::SoloOverpullCap() const
{
    uint32 const lvl = me->GetLevel();
    uint32 cap = (lvl < 5) ? 1u : (lvl < 10) ? 2u : (uint32)AIBOT_OVERPULL_SOLO;
    if (me->GetHealthPercent() < 35.0f && cap > 1u)
        --cap;   // already bloodied — bail sooner -- cb:fold hot per-update detail
    return cap;
}

bool AiBotAI::OverpullGuard(Unit* target) const
{
    if (!target)
        return false;   // cb:fold hot per-update detail

    // Neutral TARGET: attacking it doesn't proximity-aggro the field (no cascade), so the cluster
    // depth is irrelevant — it's always safe to pull. Only a reaction-hostile target risks the
    // bum-rush. (As with CountNearbyHostiles, same-faction social-assist is the lone residue, deferred.)
    if (!me->IsHostileTo(target))
        return false;   // cb:fold hot per-update detail

    // Count the OTHER will-aggro hostiles clustered with the target — that's what wakes when we
    // pull it. Solo cap for a solo bot; the GROUP cap for a grouped one (2026-07-05: no longer a
    // grouped no-op — under the one-picker doctrine the anchor's pull commits the whole team, so
    // the team-sized density cap must gate it. A camp denser than the cap = hold + patrol instead
    // of diving in; the C# grind-lock wall clock relocates a team parked against an un-pullable
    // field, so a hold here can never wedge the group forever).
    uint32 const cap = me->GetGroup() ? AIBOT_OVERPULL_GROUP : SoloOverpullCap();
    uint32 const packed = CountNearbyHostiles(target, AIBOT_PULL_DENSITY_RADIUS);
    if (packed > cap)
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: overpull guard veto, camp too dense", packed);
        return true;
    }
    return false;
}

bool AiBotAI::HandleOverpullRetreat()
{
    if (!me->IsInCombat())
    {   // cb:fold hot per-update detail
        m_overpullFleeHoldMs = 0;
        m_overpullFlees = 0;
        return false;
    }
 
    // Mid-retreat hold: let the hop run; don't let the combat AI re-chase this tick.
    if (m_overpullFleeHoldMs > 0)
    {   // cb:fold hot per-update detail
        m_overpullFleeHoldMs = (m_overpullFleeHoldMs > AIBOT_UPDATE_INTERVAL)
                             ? m_overpullFleeHoldMs - AIBOT_UPDATE_INTERVAL : 0;
        return true;
    }
 
    uint32 const cap = me->GetGroup() ? AIBOT_OVERPULL_GROUP : SoloOverpullCap();
    auto const& attackers = me->GetAttackers();   // melee attacker set (Unit*)
    uint32 const count = (uint32)attackers.size();
 
    if (count <= cap)
    {   // cb:fold hot per-update detail
        m_overpullFlees = 0;   // recovered (mobs leashed / we whittled them down)
        return false;
    }
 
    // Already bailed the max number of times and still buried — stop thrashing; let the
    // combat AI fight it out (whether that's a death or a clutch kill is now the C# planner's
    // call via the death-loop / group escalation).
    if (m_overpullFlees >= AIBOT_OVERPULL_MAX_FLEES)
    {   // cb:fold hot per-update detail
        CB_HIT(me->GetGUIDLow(), "cpp-combat: overpull flees exhausted, standing to fight");
        return false;
    }
 
    // Retreat away from the hostile centroid toward open ground; short-ignore the attackers
    // so Select*Target doesn't re-acquire them while we run.
    float cx = 0.0f, cy = 0.0f; uint32 n = 0;
    for (Unit* a : attackers)
    {
        if (!a)
            continue;   // cb:fold hot per-update detail
        cx += a->GetPositionX();
        cy += a->GetPositionY();
        ++n;
        m_combatIgnore[a->GetGUIDLow()] = AIBOT_OVERPULL_FLEE_HOLD_MS + 500;
    }
 
    float rx = me->GetPositionX(), ry = me->GetPositionY(), rz = me->GetPositionZ();
    if (n > 0)
    {   // cb:fold hot per-update detail
        cx /= n; cy /= n;
        float ax = rx - cx, ay = ry - cy;
        float len = sqrtf(ax * ax + ay * ay);
        if (len > 0.5f)
        {   // cb:fold hot per-update detail
            rx += (ax / len) * AIBOT_OVERPULL_RETREAT_DIST;
            ry += (ay / len) * AIBOT_OVERPULL_RETREAT_DIST;
        }
    }
    me->GetRandomPoint(rx, ry, rz, AIBOT_OVERPULL_RETREAT_DIST, rx, ry, rz);  // snap to valid terrain
 
    m_overpullFlees++;
    CB_HITV(me->GetGUIDLow(), "cpp-combat: overpull retreat, attackers over cap", count);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-OVERPULL] %s: %u attackers > cap %u (%s) — retreat %u/%u to (%.1f, %.1f)",
        me->GetName(), count, cap, me->GetGroup() ? "group" : "solo",
        m_overpullFlees, AIBOT_OVERPULL_MAX_FLEES, rx, ry);
 
    me->AttackStop();
    StopMoving();
    MovePointRun(AIBOT_POINT_OVERPULL_FLEE, rx, ry, rz);
    m_overpullFleeHoldMs = AIBOT_OVERPULL_FLEE_HOLD_MS;
    return true;
}

// ============================================================================
// [PULL] Proactive pull-and-retreat (FINDING_005, 2026-08-06)
//
// The old engage body-DIVED the camp: AttackStart -> MoveChase straight INTO the mob, so even a
// "least-clustered" pick woke its neighbours by proximity/social aggro, and only the REACTIVE
// HandleOverpullRetreat bailed — AFTER the overpull already happened (the 100-attacker Menethil
// pulls). BeginPull makes a fresh grind engagement a real PULL: tag the mob, then drag it back to
// an open-ground anchor so only it (+ tightly-linked neighbours) follows; fight it isolated.
// HandleOverpullRetreat stays as the in-combat backstop. Ranged tag at ~25y (their caster chase),
// melee body-tag at reach; both then retreat. Routed from the SHARED TASK_GRIND AttackStart, so
// the Solo scan AND the TeamAuto anchor both pull-and-retreat (followers hold → never self-pull).
// ============================================================================
bool AiBotAI::BeginPull(Unit* pTarget)
{
    if (!pTarget)
        return false;   // cb:fold hot per-update detail

    // Immobile targets (totems) can't be dragged — a pull-retreat just runs away from a mob that
    // won't follow, then comes back and re-tags (the StormSamurai "Strength of Earth Totem" loop).
    // Fight those in place; no pull. (FINDING_005)
    if (pTarget->IsCreature() && static_cast<Creature*>(pTarget)->IsTotem())
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: pull skipped, totem fought in place", pTarget->GetEntry());
        m_pullActive = false;
        return AttackStart(pTarget);
    }

    // Anchor = our current spot pushed AIBOT_PULL_RETREAT_DIST directly AWAY from the target
    // (open ground behind us), snapped to valid terrain. Computed BEFORE we close in.
    float rx = me->GetPositionX(), ry = me->GetPositionY(), rz = me->GetPositionZ();
    float ax = rx - pTarget->GetPositionX(), ay = ry - pTarget->GetPositionY();
    float len = sqrtf(ax * ax + ay * ay);
    if (len > 0.5f)
    {   // cb:fold hot per-update detail
        rx += (ax / len) * AIBOT_PULL_RETREAT_DIST;
        ry += (ay / len) * AIBOT_PULL_RETREAT_DIST;
    }
    me->GetRandomPoint(rx, ry, rz, AIBOT_PULL_RETREAT_DIST, rx, ry, rz);

    m_pullAnchorX = rx;
    m_pullAnchorY = ry;
    m_pullAnchorZ = rz;
    m_pullTargetGuid = pTarget->GetObjectGuid();
    m_pullTagged = false;
    m_pullRetreatHoldMs = 0;
    m_pullElapsedMs = 0;

    // Tag it. AttackStart drives the approach (melee -> reach; ranged -> ~25y caster chase) and
    // starts auto-attack/auto-shot; the mob aggroes when the first swing/shot lands, and
    // HandlePullRetreat then breaks off and drags it to the anchor. Only arm the pull if the
    // attack actually started (else leave m_pullActive false so nothing lingers).
    if (!AttackStart(pTarget))
    {   // cb:fold hot per-update detail
        CB_HIT(me->GetGUIDLow(), "cpp-combat: pull aborted, attack start refused");
        m_pullActive = false;
        return false;
    }
    m_pullActive = true;
    CB_HITV(me->GetGUIDLow(), "cpp-combat: pull begun, tag then drag to anchor", pTarget->GetEntry());
    return true;
}

bool AiBotAI::HandlePullRetreat()
{
    if (!m_pullActive)
        return false;   // cb:fold hot per-update detail

    // Combat gone (mob dead / we died / it leashed) -> pull is over; normal flow resumes.
    if (!me->IsInCombat())
    {   // cb:fold hot per-update detail
        CB_HIT(me->GetGUIDLow(), "cpp-combat: pull over, combat dropped");
        m_pullActive = false;
        return false;
    }

    // Safety timeout: never let the pull sequence run forever (e.g. an unreachable tag) -> drop to
    // normal combat, which owns its own stalemate/overpull handling.
    m_pullElapsedMs += AIBOT_UPDATE_INTERVAL;
    if (m_pullElapsedMs >= AIBOT_PULL_MAX_MS)
    {   // cb:fold hot per-update detail
        CB_HIT(me->GetGUIDLow(), "cpp-combat: pull timeout, dropping to normal combat");
        m_pullActive = false;
        return false;
    }

    Creature* pTarget = me->GetMap()->GetCreature(m_pullTargetGuid);
    if (!pTarget || !pTarget->IsAlive())
    {   // cb:fold hot per-update detail
        CB_HIT(me->GetGUIDLow(), "cpp-combat: pull target gone");
        m_pullActive = false;
        return false;
    }

    float const tagRange = (m_role == ROLE_RANGE_DPS && IsRangedDamageClass(me->GetClass()))
                         ? AIBOT_PULL_RANGED_TAG_RANGE : AIBOT_PULL_MELEE_TAG_RANGE;

    // Phase 1 -- APPROACH/TAG: let AttackStart's MoveChase close the gap; once we're within tag
    // range of our victim (swing/shot has connected -> mob aggroed), break off and retreat.
    if (!m_pullTagged)
    {   // cb:fold hot per-update detail
        bool const tagged = (me->GetVictim() == pTarget) &&
                            (me->GetCombatDistance(pTarget) <= tagRange);
        if (!tagged)
            return false;   // still closing; leave AttackStart's chase running this tick   // cb:fold hot per-update detail

        m_pullTagged = true;
        CB_HIT(me->GetGUIDLow(), "cpp-combat: pull tagged, dragging mob to anchor");
        me->AttackStop();
        StopMoving();
        MovePointRun(AIBOT_POINT_PULL_RETREAT, m_pullAnchorX, m_pullAnchorY, m_pullAnchorZ);
        m_pullRetreatHoldMs = AIBOT_PULL_RETREAT_HOLD_MS;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PULL] %s: tagged %s — dragging %.0fyd back to open ground",
            me->GetName(), pTarget->GetName(), AIBOT_PULL_RETREAT_DIST);
        return true;
    }

    // Phase 2 -- RETREAT: hold the hop. Stop early once we're back at the anchor with the mob in
    // tow, else run the hold out; then turn and fight the (now isolated) mob.
    bool const atAnchor = me->GetDistance2d(m_pullAnchorX, m_pullAnchorY) <= AIBOT_PULL_ARRIVE_RANGE;
    if (m_pullRetreatHoldMs > AIBOT_UPDATE_INTERVAL && !atAnchor)
    {   // cb:fold hot per-update detail
        m_pullRetreatHoldMs -= AIBOT_UPDATE_INTERVAL;
        return true;   // keep retreating
    }

    // Arrived (or hold expired): resume normal combat here, isolated from the camp.
    CB_HIT(me->GetGUIDLow(), "cpp-combat: pull complete, engaging isolated target");
    m_pullActive = false;
    AttackStart(pTarget);
    return true;
}

bool AiBotAI::AttackStart(Unit* pVictim)
{
    // [SUI] Manual primary: only an explicit RTS ATTACK order (m_suiOrderedAttackPass) may start
    // a fight; assist, grind, defend and the doctrine all stand down.
    if (m_suiManual && !m_suiOrderedAttackPass)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combat: attack start refused, manual primary");
        return false;
    }
    m_isBuffing = false;

    if (me->IsMounted())
        me->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);   // cb:fold hot per-update detail

    if (me->Attack(pVictim, true))
    {   // cb:fold hot per-update detail
        CB_HITV(me->GetGUIDLow(), "cpp-combat: attack start, chasing victim", pVictim->GetEntry());
        if ((m_role == ROLE_RANGE_DPS || m_role == ROLE_HEALER) &&
            IsRangedDamageClass(me->GetClass()) &&
            me->GetPowerPercent(POWER_MANA) > 10.0f &&
            me->GetCombatDistance(pVictim) > 8.0f)
            me->SetCasterChaseDistance(25.0f);   // cb:fold hot per-update detail
        else if (me->HasDistanceCasterMovement())   // cb:fold hot per-update detail
            me->SetCasterChaseDistance(0.0f);   // cb:fold hot per-update detail

        me->GetMotionMaster()->MoveChase(pVictim, 1.0f, m_role == ROLE_MELEE_DPS ? 3.0f : 0.0f);
        return true;
    }

    return false;
}

Unit* AiBotAI::SelectAttackTarget(Unit* pExcept) const
{
    // [DOCTRINE] SelectAttackTarget is now the PURE solo re-pick. The group focus-fire override
    // that used to prefix it here moved into the TeamAuto doctrine's MaintainTarget
    // (AiBotDoctrineTeam.cpp) — the single in-combat target authority — so this method runs only
    // as the Solo / defer fallback. The pExcept contract (never return the unit the caller is
    // switching away from) is unchanged for that solo path.

    // 1. Check units we are currently in combat with.

    std::list<Unit*> targets;
    HostileReference* pReference = me->GetHostileRefManager().getFirst();

    while (pReference)
    {
        if (Unit* pTarget = pReference->getSourceUnit())
        {   // cb:fold hot per-update detail
            if (pTarget != pExcept &&
                IsValidHostileTarget(pTarget) &&
                !IsCombatIgnored(pTarget->GetGUIDLow()) &&
                me->IsWithinDist(pTarget, VISIBILITY_DISTANCE_NORMAL))
            {   // cb:fold hot per-update detail
                targets.push_back(pTarget);
            }
        }
        pReference = pReference->next();
    }

    if (!targets.empty())
    {   // cb:fold hot per-update detail
        targets.sort([this](Unit* pUnit1, const Unit* pUnit2)
        {
            return me->GetDistance(pUnit1) < me->GetDistance(pUnit2);
        });

        CB_HITV(me->GetGUIDLow(), "cpp-combat: target acquired, nearest combat ref", (*targets.begin())->GetEntry());
        return *targets.begin();
    }

    // 2. Find nearby hostile creatures (mobs that aggro us in the open world).

    float const maxAggroDistance = GetMaxAggroDistanceForMap();

    // 3. Check party attackers.

    if (Group* pGroup = me->GetGroup())
    {   // cb:fold hot per-update detail
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* pMember = itr->getSource())
            {   // cb:fold hot per-update detail
                if (pMember == me)
                    continue;   // cb:fold hot per-update detail

                if (me->GetDistance(pMember) > 30.0f)
                    continue;   // cb:fold hot per-update detail

                if (Unit* pAttacker = pMember->GetAttackerForHelper())
                {   // cb:fold hot per-update detail
                    if (pAttacker != pExcept &&
                        IsValidHostileTarget(pAttacker) &&
                        !IsCombatIgnored(pAttacker->GetGUIDLow()) &&
                        me->IsWithinDist(pAttacker, maxAggroDistance * 2.0f) &&
                        me->GetDistanceZ(pAttacker) < 10.0f &&
                        me->IsWithinLOSInMap(pAttacker))
                    {   // cb:fold hot per-update detail
                        CB_HITV(me->GetGUIDLow(), "cpp-combat: target acquired, assisting group member", pAttacker->GetEntry());
                        return pAttacker;
                    }
                }
            }
        }
    }

    return nullptr;
}

bool AiBotAI::CheckForUnreachableTarget()
{
    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE &&
       !me->GetMotionMaster()->GetCurrent()->IsReachable())
    {   // cb:fold hot per-update detail
        if (Unit* pTarget = static_cast<ChaseMovementGenerator<Player> const*>(me->GetMotionMaster()->GetCurrent())->GetTarget())
        {   // cb:fold hot per-update detail
            if (!me->CanReachWithMeleeAutoAttack(pTarget))
            {   // cb:fold hot per-update detail
                if (!me->IsWithinDist(pTarget, VISIBILITY_DISTANCE_NORMAL))
                {   // cb:fold hot per-update detail
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: unreachable target dropped, out of visibility");
                    me->AttackStop(false);
                    StopMoving();
                    return true;
                }

                // [REMESH] If WE are the unreachable one (off the mesh), walk back on before any cheat.
                if (!me->IsMoving() && TryRemeshStep("unreachable-target"))
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: unreachable target, bot was off-mesh, remesh step issued");
                    return true;
                }

                if (pTarget->IsCreature() && !me->IsMoving())
                {   // cb:fold hot per-update detail
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: unreachable target, teleporting onto mob");
                    // Cheating to prevent getting stuck because of bad mmaps.
                    // [GROUND] Teleport onto the mob, grounded. A ground-spawned mob is a
                    // no-op; this only bites if the chase target itself sits on a float.
                    // Switched from NearTeleportTo(GetPosition()) to the 4-arg form so the
                    // bot keeps its own facing (irrelevant for melee — combat sets facing).
                    float tx = pTarget->GetPositionX();
                    float ty = pTarget->GetPositionY();
                    float tz = pTarget->GetPositionZ();
                    ReGroundZ(tx, ty, tz, "unreach-tp");
                    me->NearTeleportTo(tx, ty, tz, me->GetOrientation());
                    return true;
                }

                if (me->GetDistanceZ(pTarget) > 10.0f)
                {   // cb:fold hot per-update detail
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: unreachable target above or below, attack stopped");
                    me->AttackStop(false);
                    StopMoving();
                    return true;
                }
            }
        }
    }

    return false;
}

// ============================================================
// 18 COMBAT METHODS + DISPATCHERS (verbatim from BattleBotAI,
// class name replaced: BattleBotAI -> AiBotAI)
// ============================================================

void AiBotAI::UpdateOutOfCombatAI()
{
    // People, not animals, between fights (owner 2026-08-25): combat forms
    // drop once the fight has been over for a grace period, so the fleet
    // reads as characters while questing/idle. Travel/Aquatic forms are
    // journey tools and stay; the next fight re-shifts per role/spec.
    if (me->GetClass() == CLASS_DRUID)
    {   // cb:fold hot per-update detail
        ShapeshiftForm const form = me->GetShapeshiftForm();
        if ((form == FORM_BEAR || form == FORM_DIREBEAR || form == FORM_CAT ||
             form == FORM_MOONKIN) &&
            WorldTimer::getMSTimeDiff(m_lastCombatMs, WorldTimer::getMSTime()) > 8000)
            me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);   // cb:fold hot per-update detail
    }

    // A validated persisted profile owns its deterministic OOC maintenance.
    // Unknown/conflict/invalid/error profiles intentionally fall through to
    // the proven legacy class behavior below.
    if (UpdateSpecOutOfCombatAI())
        return;   // cb:fold hot per-update detail

    switch (me->GetClass())
    {
        case CLASS_PALADIN:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Shaman();
            break;
        case CLASS_HUNTER:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Hunter();
            break;
        case CLASS_MAGE:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Mage();
            break;
        case CLASS_PRIEST:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Priest();
            break;
        case CLASS_WARLOCK:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Warrior();
            break;
        case CLASS_ROGUE:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Rogue();
            break;
        case CLASS_DRUID:   // cb:fold hot per-update detail
            UpdateOutOfCombatAI_Druid();
            break;
    }
}

void AiBotAI::UpdateInCombatAI()
{
    m_lastCombatMs = WorldTimer::getMSTime();
    // [ROTATION] A loaded slate OWNS in-combat casting — the class switch below is the
    // vanilla else-branch (RotationSlate design, 2026-05-11). The 250ms sub-tick in
    // UpdateAI is the main driver; this 1s call is just one more evaluation, so a bot
    // whose sub-tick guards skipped (e.g. mid-cast) still gets the behaviour-tick try.
    if (!m_rotation.empty())
    {   // cb:fold hot per-update detail
        UpdateRotationSlate();
        if (me->GetVictim())
            UseTrinketEffects();   // cb:fold hot per-update detail
        return;
    }

    if (UpdateSpecCombatAI())
    {   // cb:fold hot per-update detail
        if (me->GetVictim())
            UseTrinketEffects();   // cb:fold hot per-update detail
        return;
    }

    switch (me->GetClass())
    {
        case CLASS_PALADIN:   // cb:fold hot per-update detail
            UpdateInCombatAI_Paladin();
            break;
        case CLASS_SHAMAN:   // cb:fold hot per-update detail
            UpdateInCombatAI_Shaman();
            break;
        case CLASS_HUNTER:   // cb:fold hot per-update detail
            UpdateInCombatAI_Hunter();
            break;
        case CLASS_MAGE:   // cb:fold hot per-update detail
            UpdateInCombatAI_Mage();
            break;
        case CLASS_PRIEST:   // cb:fold hot per-update detail
            UpdateInCombatAI_Priest();
            break;
        case CLASS_WARLOCK:   // cb:fold hot per-update detail
            UpdateInCombatAI_Warlock();
            break;
        case CLASS_WARRIOR:   // cb:fold hot per-update detail
            UpdateInCombatAI_Warrior();
            break;
        case CLASS_ROGUE:   // cb:fold hot per-update detail
            UpdateInCombatAI_Rogue();
            break;
        case CLASS_DRUID:   // cb:fold hot per-update detail
            UpdateInCombatAI_Druid();
            break;
    }

    if (me->GetVictim())
        UseTrinketEffects();   // cb:fold hot per-update detail
}

// ============================================================================
// [ROTATION] The slate evaluator (2026-07-16; RotationSlate design 2026-05-11).
//
// Walk the priority-sorted instructions; the FIRST one whose target resolves,
// whose HP window and aura condition pass, and that CanTryToCastSpell accepts,
// is cast — first match wins the tick, exactly the design. Called from the
// 250ms sub-tick in UpdateAI and from UpdateInCombatAI; both gate on IsAlive/
// InCombat/not-casting before reaching here, so this stays lean for 4 Hz.
//
// Notes locked at build time:
//  - pSpell was resolved at LOAD (unknown/unlearned spells are null → skipped);
//  - CanTryToCastSpell owns GCD / cooldown / power / range; DoCastSpell owns
//    stopping to cast — the same primitives every class rotation trusts;
//  - an active autorepeat of the SAME spell is skipped (don't restart the wand
//    every sub-tick), but a DIFFERENT castable instruction interrupts wanding
//    naturally via DoCastSpell — the wand-trap cure;
//  - returns true whenever a slate is present: slate present == combat handled,
//    even on a tick where nothing was castable (the vanilla switch must not run).
// ============================================================================
bool AiBotAI::UpdateRotationSlate()
{
    if (m_rotation.empty())
        return false;   // cb:fold rotation rung, outcome probed at cast

    for (auto const& inst : m_rotation)
    {
        SpellEntry const* pSpell = inst.pSpell;
        // Defense in depth: a talent reset or any later spell loss must never
        // leave a cached slate pointer capable of casting an unlearned spell.
        if (!pSpell || !me->HasSpell(inst.spellId))
            continue;   // cb:fold rotation rung, outcome probed at cast

        // Already wanding/shooting this exact spell — let the autorepeat run.
        if (Spell* pAuto = me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            if (pAuto->m_spellInfo == pSpell)   // cb:fold rotation rung, outcome probed at cast
                continue;   // cb:fold rotation rung, outcome probed at cast

        Unit* pTarget = ResolveRotationTarget(inst.target);
        if (!pTarget || !pTarget->IsAlive())
            continue;   // cb:fold rotation rung, outcome probed at cast

        float const hp = pTarget->GetHealthPercent();
        if (hp < (float)inst.hpMin || hp > (float)inst.hpMax)
            continue;   // cb:fold rotation rung, outcome probed at cast

        if (inst.auraId)
        {   // cb:fold rotation rung, outcome probed at cast
            bool const has = pTarget->HasAura(inst.auraId);
            if (has != inst.auraPresent)
                continue;   // cb:fold rotation rung, outcome probed at cast
        }

        if (!CanTryToCastSpell(pTarget, pSpell))
            continue;   // cb:fold rotation rung, outcome probed at cast

        if (DoCastSpell(pTarget, pSpell) == SPELL_CAST_OK)
        {   // cb:fold rotation rung, outcome probed at cast
            CB_HITV(me->GetGUIDLow(), "cpp-combat: rotation slate winner cast", inst.spellId);
            return true;
        }
    }

    return true;   // slate present — combat handled even when nothing fired this tick
}

// [ROTATION] Target kinds the slate can name. 0=SELF, 1=CURRENT_TARGET (the spine/
// doctrine-owned victim — the slate never picks fights, it only executes on them),
// 2=LOWEST_HP_PARTY (lowest-HP% living group player within 40yd, self included; solo
// bots resolve to self, and the instruction's own hpMax window gates whether a heal
// actually fires). Unknown kinds resolve null and the instruction is skipped.
Unit* AiBotAI::ResolveRotationTarget(uint8 kind)
{
    switch (kind)
    {
        case 0:   // cb:fold hot per-update detail
            return me;
        case 1:   // cb:fold hot per-update detail
            return me->GetVictim();
        case 2:   // cb:fold hot per-update detail
        {
            Group* pGroup = me->GetGroup();
            if (!pGroup)
                return me;   // cb:fold hot per-update detail
            Unit* pBest = nullptr;
            float bestHp = 101.0f;
            for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* pMember = itr->getSource();
                if (!pMember || !pMember->IsAlive() || !pMember->IsInWorld())
                    continue;   // cb:fold hot per-update detail
                if (pMember->GetMapId() != me->GetMapId())
                    continue;   // cb:fold hot per-update detail
                if (pMember != me && !me->IsWithinDist(pMember, 40.0f))
                    continue;   // cb:fold hot per-update detail
                float const hp = pMember->GetHealthPercent();
                if (hp < bestHp)
                {   // cb:fold hot per-update detail
                    bestHp = hp;
                    pBest = pMember;
                }
            }
            return pBest ? pBest : me;
        }
    }
    return nullptr;
}

void AiBotAI::UpdateOutOfCombatAI_Paladin()
{
    if (m_spells.paladin.pAura &&
        CanTryToCastSpell(me, m_spells.paladin.pAura))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.paladin.pAura) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.paladin.pBlessingBuff)
    {   // cb:fold rotation rung, outcome probed at cast
        if (Player* pTarget = SelectBuffTarget(m_spells.paladin.pBlessingBuff))
        {   // cb:fold rotation rung, outcome probed at cast
            if (CanTryToCastSpell(pTarget, m_spells.paladin.pBlessingBuff))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pTarget, m_spells.paladin.pBlessingBuff) == SPELL_CAST_OK)
                {   // cb:fold rotation rung, outcome probed at cast
                    m_isBuffing = true;
                    return;
                }
            }
        }
    }

    if (m_isBuffing &&
       (!m_spells.paladin.pBlessingBuff ||
        !me->HasGCD(m_spells.paladin.pBlessingBuff)))
    {   // cb:fold rotation rung, outcome probed at cast
        m_isBuffing = false;
    }

    FindAndHealInjuredAlly();
}

void AiBotAI::UpdateInCombatAI_Paladin()
{
    if (m_spells.paladin.pDivineShield &&
       (me->GetHealthPercent() < 20.0f) &&
       (me->GetPowerPercent(POWER_MANA) > 40.0f) &&
       !me->HasAura(AURA_WARSONG_FLAG) &&
        CanTryToCastSpell(me, m_spells.paladin.pDivineShield))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.paladin.pDivineShield) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    bool const hasSeal = m_spells.paladin.pSeal && me->HasAura(m_spells.paladin.pSeal->Id);

    if (!hasSeal &&
        m_spells.paladin.pSeal &&
        CanTryToCastSpell(me, m_spells.paladin.pSeal))
    {   // cb:fold rotation rung, outcome probed at cast
        me->CastSpell(me, m_spells.paladin.pSeal, false);
    }

    // Holy Shock is a triage spell first.  The legacy ordering tried offense
    // before it ever looked for an injured ally.
    if (m_spells.paladin.pHolyShock)
    {   // cb:fold rotation rung, outcome probed at cast
        if (Unit* pHeal = SelectHealTarget(65.0f, 65.0f))
        {   // cb:fold rotation rung, outcome probed at cast
            if (CanTryToCastSpell(pHeal, m_spells.paladin.pHolyShock) &&
                DoCastSpell(pHeal, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (hasSeal && m_spells.paladin.pJudgement &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pJudgement))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.paladin.pJudgement) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pHammerOfJustice &&
            pVictim->IsNonMeleeSpellCasted() &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfJustice))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfJustice) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pHammerOfWrath &&
            pVictim->GetHealthPercent() < 20.0f &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pHammerOfWrath))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.paladin.pHammerOfWrath) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pHolyShield &&
            CanTryToCastSpell(me, m_spells.paladin.pHolyShield) &&
           (IsMeleeDamageClass(pVictim->GetClass()) || (GetAttackersInRangeCount(8.0f) > 1)))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.paladin.pHolyShield) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pConsecration &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(me, m_spells.paladin.pConsecration))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.paladin.pConsecration) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pHolyShock &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pHolyShock))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.paladin.pDivineFavor &&
                CanTryToCastSpell(me, m_spells.paladin.pDivineFavor))
            {   // cb:fold rotation rung, outcome probed at cast
                DoCastSpell(me, m_spells.paladin.pDivineFavor);
            }

            if (DoCastSpell(pVictim, m_spells.paladin.pHolyShock) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pExorcism &&
            pVictim->IsCreature() &&
           (pVictim->GetCreatureType() == CREATURE_TYPE_UNDEAD) &&
            CanTryToCastSpell(pVictim, m_spells.paladin.pExorcism))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.paladin.pExorcism) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
           !me->CanReachWithMeleeAutoAttack(pVictim))
        {   // cb:fold rotation rung, outcome probed at cast
            me->GetMotionMaster()->MoveChase(pVictim);
        }
    }

    if (Unit* pFriend = me->FindLowestHpFriendlyUnit(30.0f, 70, true, me))
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.paladin.pBlessingOfProtection &&
           !IsPhysicalDamageClass(pFriend->GetClass()) &&
           !pFriend->HasAura(AURA_WARSONG_FLAG) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfProtection) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pBlessingOfSacrifice &&
            pFriend->HasAura(AURA_WARSONG_FLAG) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pFriend, m_spells.paladin.pBlessingOfSacrifice) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
        if (m_spells.paladin.pLayOnHands &&
           (pFriend->GetHealthPercent() < 15.0f) &&
            CanTryToCastSpell(pFriend, m_spells.paladin.pLayOnHands))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pFriend, m_spells.paladin.pLayOnHands) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    if (m_spells.paladin.pBlessingOfFreedom &&
       (me->HasUnitState(UNIT_STATE_ROOT) || me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
        CanTryToCastSpell(me, m_spells.paladin.pBlessingOfFreedom))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.paladin.pBlessingOfFreedom) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.paladin.pCleanse)
    {   // cb:fold rotation rung, outcome probed at cast
        if (Unit* pFriend = SelectDispelTarget(m_spells.paladin.pCleanse))
        {   // cb:fold rotation rung, outcome probed at cast
            if (CanTryToCastSpell(pFriend, m_spells.paladin.pCleanse))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pFriend, m_spells.paladin.pCleanse) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }
    }

    FindAndHealInjuredAlly(me->IsTotalImmune() ? 80.0f : 40.0f, 50.0f);
}

void AiBotAI::UpdateOutOfCombatAI_Shaman()
{
    if (m_spells.shaman.pWeaponBuff &&
        CanTryToCastSpell(me, m_spells.shaman.pWeaponBuff))
    {   // cb:fold rotation rung, outcome probed at cast
        if (CastWeaponBuff(m_spells.shaman.pWeaponBuff, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.shaman.pLightningShield &&
        CanTryToCastSpell(me, m_spells.shaman.pLightningShield))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.shaman.pLightningShield) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (SummonShamanTotems())
            return;   // cb:fold rotation rung, outcome probed at cast

        UpdateInCombatAI_Shaman();
    }
    else
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.shaman.pGhostWolf &&
           !me->IsMoving() && !me->IsMounted() &&
           (!GetMountSpellId() || me->HasAura(AURA_WARSONG_FLAG) || me->HasAura(AURA_SILVERWING_FLAG)) &&
            CanTryToCastSpell(me, m_spells.shaman.pGhostWolf))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.shaman.pGhostWolf) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
}

void AiBotAI::UpdateInCombatAI_Shaman()
{
    if (m_spells.shaman.pGhostWolf &&
        me->GetShapeshiftForm() == FORM_GHOSTWOLF)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combat: ghost wolf dropped for combat");
        me->RemoveAurasDueToSpellByCancel(m_spells.shaman.pGhostWolf->Id);
    }

    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.shaman.pManaTideTotem &&
           (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
            CanTryToCastSpell(me, m_spells.shaman.pManaTideTotem))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.shaman.pManaTideTotem) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pElementalMastery &&
            me->GetAttackers().empty() &&
            CanTryToCastSpell(me, m_spells.shaman.pElementalMastery))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.shaman.pElementalMastery) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pEarthShock &&
            pVictim->IsNonMeleeSpellCasted(false, false, true) &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pEarthShock))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pEarthShock) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pFrostShock &&
            pVictim->IsMoving() &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pFrostShock))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pFrostShock) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pStormstrike &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pStormstrike))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pStormstrike) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pChainLightning &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pChainLightning))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pChainLightning) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pPurge &&
            IsValidDispelTarget(pVictim, m_spells.shaman.pPurge) &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pPurge))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pPurge) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pFlameShock &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pFlameShock))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pFlameShock) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.shaman.pLightningBolt &&
           !me->CanReachWithMeleeAutoAttack(pVictim) &&
            CanTryToCastSpell(pVictim, m_spells.shaman.pLightningBolt))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.shaman.pLightningBolt) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    if (SummonShamanTotems())
        return;   // cb:fold rotation rung, outcome probed at cast

    if (m_spells.shaman.pCureDisease &&
        CanTryToCastSpell(me, m_spells.shaman.pCureDisease) &&
        IsValidDispelTarget(me, m_spells.shaman.pCureDisease))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.shaman.pCureDisease) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.shaman.pCurePoison &&
        CanTryToCastSpell(me, m_spells.shaman.pCurePoison) &&
        IsValidDispelTarget(me, m_spells.shaman.pCurePoison))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.shaman.pCurePoison) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    FindAndHealInjuredAlly(40.0f);
}

void AiBotAI::UpdateOutOfCombatAI_Hunter()
{
    if (m_spells.hunter.pAspectOfTheCheetah &&
       !me->IsMounted() &&
        CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheCheetah))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.hunter.pAspectOfTheCheetah) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.hunter.pHuntersMark &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pHuntersMark))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.hunter.pHuntersMark) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (Pet* pPet = me->GetPet())
        {   // cb:fold rotation rung, outcome probed at cast
            if (!pPet->GetVictim())
            {   // cb:fold rotation rung, outcome probed at cast
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Hunter();
    }
}

void AiBotAI::UpdateInCombatAI_Hunter()
{
    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {   // cb:fold rotation rung, outcome probed at cast
            me->GetMotionMaster()->MoveChase(pVictim, 25.0f);
        }

        if (me->HasSpell(AB_SPELL_AUTO_SHOT) &&
            !me->IsMoving() &&
            (me->GetCombatDistance(pVictim) > 8.0f) &&
            !me->IsNonMeleeSpellCasted())
        {   // cb:fold rotation rung, outcome probed at cast
            switch (me->CastSpell(pVictim, AB_SPELL_AUTO_SHOT, false))
            {
                case SPELL_FAILED_NEED_AMMO:   // cb:fold shares the restock probe below
                case SPELL_FAILED_NO_AMMO:   // cb:fold rotation rung, outcome probed at cast
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: out of ammo, restocking");
                    AddHunterAmmo();
                    break;
                }
            }
        }

        if (m_spells.hunter.pConcussiveShot &&
            pVictim->IsMoving() && (pVictim->GetVictim() == me) &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pConcussiveShot))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.hunter.pConcussiveShot) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.hunter.pAimedShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pAimedShot))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.hunter.pAimedShot) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.hunter.pArcaneShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pArcaneShot))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.hunter.pArcaneShot) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.hunter.pSerpentSting &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pSerpentSting))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.hunter.pSerpentSting) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.hunter.pMultiShot &&
            CanTryToCastSpell(pVictim, m_spells.hunter.pMultiShot))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.hunter.pMultiShot) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.hunter.pAspectOfTheCheetah &&
            me->HasAura(m_spells.hunter.pAspectOfTheCheetah->Id))
        {   // cb:fold rotation rung, outcome probed at cast
            if (pVictim->CanReachWithMeleeAutoAttack(me))
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.hunter.pAspectOfTheMonkey &&
                    CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheMonkey))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.hunter.pAspectOfTheMonkey) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
            else
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.hunter.pAspectOfTheHawk &&
                    CanTryToCastSpell(me, m_spells.hunter.pAspectOfTheHawk))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.hunter.pAspectOfTheHawk) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }

        if (pVictim->CanReachWithMeleeAutoAttack(me))
        {   // cb:fold rotation rung, outcome probed at cast
            if (me->HasUnitState(UNIT_STATE_ROOT))
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.hunter.pMongooseBite &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pMongooseBite))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.hunter.pMongooseBite) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.hunter.pRaptorStrike &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pRaptorStrike))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.hunter.pRaptorStrike) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
            else
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.hunter.pWingClip &&
                    CanTryToCastSpell(pVictim, m_spells.hunter.pWingClip))
                {   // cb:fold rotation rung, outcome probed at cast
                    DoCastSpell(pVictim, m_spells.hunter.pWingClip);
                }
            }
        }

        if (!me->HasUnitState(UNIT_STATE_ROOT) &&
            (me->GetCombatDistance(pVictim) < 8.0f) &&
             me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE)
        {   // cb:fold rotation rung, outcome probed at cast
            CB_HIT(me->GetGUIDLow(), "cpp-combat: hunter opening range from melee");
            if (!me->IsStopped())
                me->StopMoving();   // cb:fold hot per-update detail
            me->GetMotionMaster()->Clear();
            if (me->GetMotionMaster()->MoveDistance(pVictim, 25.0f))
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
}

void AiBotAI::UpdateOutOfCombatAI_Mage()
{
    if (m_spells.mage.pArcaneBrilliance)
    {   // cb:fold rotation rung, outcome probed at cast
        if (CanTryToCastSpell(me, m_spells.mage.pArcaneBrilliance))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pArcaneBrilliance) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
    else if (m_spells.mage.pArcaneIntellect)
    {   // cb:fold rotation rung, outcome probed at cast
        if (CanTryToCastSpell(me, m_spells.mage.pArcaneIntellect))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pArcaneIntellect) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    if (m_spells.mage.pIceArmor &&
        CanTryToCastSpell(me, m_spells.mage.pIceArmor))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.mage.pIceArmor) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.mage.pIceBarrier &&
        CanTryToCastSpell(me, m_spells.mage.pIceBarrier))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.mage.pIceBarrier) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (me->GetVictim())
        UpdateInCombatAI_Mage();   // cb:fold rotation rung, outcome probed at cast
}

void AiBotAI::UpdateInCombatAI_Mage()
{
    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.mage.pCombustion &&
            CanTryToCastSpell(me, m_spells.mage.pCombustion))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pCombustion) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pPyroblast &&
            m_spells.mage.pPresenceOfMind &&
            me->HasAura(m_spells.mage.pPresenceOfMind->Id) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pPyroblast))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.mage.pPyroblast) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pIceBlock &&
           (me->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pIceBlock))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pIceBlock) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pManaShield &&
            IsPhysicalDamageClass(pVictim->GetClass()) &&
           (me->GetPowerPercent(POWER_MANA) > 20.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pManaShield))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pManaShield) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pCounterspell &&
            pVictim->IsNonMeleeSpellCasted(false, false, true) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pCounterspell))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.mage.pCounterspell) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {   // cb:fold rotation rung, outcome probed at cast
            me->GetMotionMaster()->MoveChase(pVictim, 25.0f);
        }
        else if (pVictim->CanReachWithMeleeAutoAttack(me) &&
                (pVictim->GetVictim() == me) &&
                (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.mage.pConeofCold &&
                CanTryToCastSpell(me, m_spells.mage.pConeofCold))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.mage.pConeofCold) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.mage.pBlink &&
               (me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE) ||
                me->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED)) &&
                CanTryToCastSpell(me, m_spells.mage.pBlink))
            {   // cb:fold rotation rung, outcome probed at cast
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
                    me->GetMotionMaster()->MoveIdle();   // cb:fold rotation rung, outcome probed at cast

                if (DoCastSpell(me, m_spells.mage.pBlink) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (!me->HasUnitState(UNIT_STATE_CAN_NOT_MOVE))
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.mage.pFrostNova &&
                    !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
                    !pVictim->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL) &&
                    CanTryToCastSpell(me, m_spells.mage.pFrostNova))
                {   // cb:fold rotation rung, outcome probed at cast
                    DoCastSpell(me, m_spells.mage.pFrostNova);
                }

                // [STOP-AND-CAST] Do NOT kite. Stand where we are and let the nuke rotation below
                // fire, instead of running 25yd away every time a mob reaches melee (the "mages keep
                // running away from their target" bug). Frost Nova above still roots the adjacent mob.
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
                    me->GetMotionMaster()->MoveIdle();   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (GetAttackersInRangeCount(10.0f) > 1)
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.mage.pBlastWave &&
                CanTryToCastSpell(me, m_spells.mage.pBlastWave))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.mage.pBlastWave) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.mage.pArcaneExplosion &&
                CanTryToCastSpell(me, m_spells.mage.pArcaneExplosion))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.mage.pArcaneExplosion) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        // [STOP-AND-CAST] (kite removed above) — always fall through to the nuke rotation; never
        // skip casting to finish a run-away.

        if (m_spells.mage.pRemoveLesserCurse &&
           (me->GetAttackers().size() < 3) &&
            CanTryToCastSpell(me, m_spells.mage.pRemoveLesserCurse) &&
            IsValidDispelTarget(me, m_spells.mage.pRemoveLesserCurse))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pRemoveLesserCurse) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pPolymorph)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pTarget = SelectSafeSpecAdd(pVictim))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.mage.pPolymorph))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.mage.pPolymorph) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }

        if (m_spells.mage.pArcanePower &&
           (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pArcanePower))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pArcanePower) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pPresenceOfMind &&
           (me->GetPowerPercent(POWER_MANA) > 50.0f) &&
            CanTryToCastSpell(me, m_spells.mage.pPresenceOfMind))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pPresenceOfMind) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pScorch &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.mage.pScorch))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.mage.pScorch) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pFrostbolt &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFrostbolt))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.mage.pFrostbolt) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pFireBlast &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireBlast))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.mage.pFireBlast) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pFireball &&
            CanTryToCastSpell(pVictim, m_spells.mage.pFireball))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.mage.pFireball) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.mage.pEvocation &&
           (me->GetPowerPercent(POWER_MANA) < 30.0f) &&
           (GetAttackersInRangeCount(10.0f) == 0) &&
            CanTryToCastSpell(me, m_spells.mage.pEvocation))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.mage.pEvocation) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->HasSpell(AB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, AB_SPELL_SHOOT_WAND, false);   // cb:fold rotation rung, outcome probed at cast
    }
}

void AiBotAI::UpdateOutOfCombatAI_Priest()
{
    BattleGround* bg = me->GetBattleGround();
    if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.priest.pPrayerofFortitude)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Player* pTarget = SelectBuffTarget(m_spells.priest.pPrayerofFortitude))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.priest.pPrayerofFortitude))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.priest.pPrayerofFortitude) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_spells.priest.pPrayerofSpirit)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Player* pTarget = SelectBuffTarget(m_spells.priest.pPrayerofSpirit))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.priest.pPrayerofSpirit))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.priest.pPrayerofSpirit) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_spells.priest.pShadowProtection)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Player* pTarget = SelectBuffTarget(m_spells.priest.pShadowProtection))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.priest.pShadowProtection))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.priest.pShadowProtection) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }
    }
    else if (bg && bg->GetStatus() == STATUS_IN_PROGRESS)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.priest.pPowerWordFortitude &&
            IsValidBuffTarget(me, m_spells.priest.pPowerWordFortitude) &&
            CanTryToCastSpell(me, m_spells.priest.pPowerWordFortitude))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.priest.pPowerWordFortitude) == SPELL_CAST_OK)
            {   // cb:fold rotation rung, outcome probed at cast
                m_isBuffing = true;
                return;
            }
        }

        if (m_spells.priest.pDivineSpirit &&
            IsValidBuffTarget(me, m_spells.priest.pDivineSpirit) &&
            CanTryToCastSpell(me, m_spells.priest.pDivineSpirit))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.priest.pDivineSpirit) == SPELL_CAST_OK)
            {   // cb:fold rotation rung, outcome probed at cast
                m_isBuffing = true;
                return;
            }
        }
    }

    if (m_spells.priest.pInnerFire &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFire))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.priest.pInnerFire) == SPELL_CAST_OK)
        {   // cb:fold rotation rung, outcome probed at cast
            m_isBuffing = true;
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.priest.pPowerWordFortitude ||
        !me->HasGCD(m_spells.priest.pPowerWordFortitude)))
    {   // cb:fold rotation rung, outcome probed at cast
        m_isBuffing = false;
    }

    if (me->GetVictim())
        UpdateInCombatAI_Priest();   // cb:fold rotation rung, outcome probed at cast
}

void AiBotAI::UpdateInCombatAI_Priest()
{
    if (m_spells.priest.pPowerWordShield &&
        CanTryToCastSpell(me, m_spells.priest.pPowerWordShield))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.priest.pPowerWordShield) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.priest.pInnerFocus &&
       (me->GetPowerPercent(POWER_MANA) < 50.0f) &&
        CanTryToCastSpell(me, m_spells.priest.pInnerFocus))
    {   // cb:fold rotation rung, outcome probed at cast
        DoCastSpell(me, m_spells.priest.pInnerFocus);
    }

    // Heal
    if (me->GetShapeshiftForm() == FORM_NONE &&
        FindAndHealInjuredAlly(40.0f))
        return;   // cb:fold rotation rung, outcome probed at cast

    // Dispels
    if (m_spells.priest.pDispelMagic)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_role == ROLE_HEALER)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pDispelMagic))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pFriend, m_spells.priest.pDispelMagic))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pFriend, m_spells.priest.pDispelMagic) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }
        else if (IsValidDispelTarget(me, m_spells.priest.pDispelMagic) &&
                 CanTryToCastSpell(me, m_spells.priest.pDispelMagic))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.priest.pDispelMagic) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
    if (m_spells.priest.pAbolishDisease)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_role == ROLE_HEALER)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pFriend = SelectDispelTarget(m_spells.priest.pAbolishDisease))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pFriend, m_spells.priest.pAbolishDisease))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pFriend, m_spells.priest.pAbolishDisease) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }
        else if (IsValidDispelTarget(me, m_spells.priest.pAbolishDisease) &&
                 CanTryToCastSpell(me, m_spells.priest.pAbolishDisease))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.priest.pAbolishDisease) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    // Attack
    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.priest.pShadowform &&
            CanTryToCastSpell(me, m_spells.priest.pShadowform))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.priest.pShadowform) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pSilence &&
            pVictim->IsNonMeleeSpellCasted() &&
            CanTryToCastSpell(pVictim, m_spells.priest.pSilence))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pSilence) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pVampiricEmbrace &&
            CanTryToCastSpell(pVictim, m_spells.priest.pVampiricEmbrace))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pVampiricEmbrace) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pMindBlast &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindBlast))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pMindBlast) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pShadowWordPain &&
            CanTryToCastSpell(pVictim, m_spells.priest.pShadowWordPain))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pShadowWordPain) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pDevouringPlague &&
            CanTryToCastSpell(pVictim, m_spells.priest.pDevouringPlague))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pDevouringPlague) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pPsychicScream &&
            GetAttackersInRangeCount(10.0f) &&
            CanTryToCastSpell(me, m_spells.priest.pPsychicScream))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.priest.pPsychicScream) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pManaBurn &&
           (pVictim->GetPowerType() == POWER_MANA) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pManaBurn))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pManaBurn) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.priest.pMindFlay &&
           (!GetAttackersInRangeCount(10.0f) || me->HasAuraType(SPELL_AURA_SCHOOL_ABSORB)) &&
            CanTryToCastSpell(pVictim, m_spells.priest.pMindFlay))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.priest.pMindFlay) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {   // cb:fold rotation rung, outcome probed at cast
            me->GetMotionMaster()->MoveChase(pVictim, 25.0f);
        }

        if (me->GetShapeshiftForm() == FORM_NONE)
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.priest.pHolyNova &&
                GetAttackersInRangeCount(10.0f) > 2 &&
                CanTryToCastSpell(me, m_spells.priest.pHolyNova))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.priest.pHolyNova) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.priest.pSmite &&
                CanTryToCastSpell(pVictim, m_spells.priest.pSmite))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.priest.pSmite) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (me->HasSpell(AB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, AB_SPELL_SHOOT_WAND, false);   // cb:fold rotation rung, outcome probed at cast
    }

}

void AiBotAI::UpdateOutOfCombatAI_Warlock()
{
    BattleGround* bg = me->GetBattleGround();
    if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.warlock.pDetectInvisibility)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Player* pTarget = SelectBuffTarget(m_spells.warlock.pDetectInvisibility))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.warlock.pDetectInvisibility))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.warlock.pDetectInvisibility) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }
    }

    if (m_spells.warlock.pDemonArmor &&
        CanTryToCastSpell(me, m_spells.warlock.pDemonArmor))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.warlock.pDemonArmor) == SPELL_CAST_OK)
        {   // cb:fold rotation rung, outcome probed at cast
            m_isBuffing = true;
            return;
        }
    }

    if (m_isBuffing &&
       (!m_spells.warlock.pDetectInvisibility ||
        !me->HasGCD(m_spells.warlock.pDetectInvisibility)))
    {   // cb:fold rotation rung, outcome probed at cast
        m_isBuffing = false;
    }

    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (Pet* pPet = me->GetPet())
        {   // cb:fold rotation rung, outcome probed at cast
            if (!pPet->GetVictim())
            {   // cb:fold rotation rung, outcome probed at cast
                pPet->GetCharmInfo()->SetIsCommandAttack(true);
                pPet->AI()->AttackStart(pVictim);
            }
        }

        UpdateInCombatAI_Warlock();
    }
    else
        SummonPetIfNeeded();   // cb:fold rotation rung, outcome probed at cast
}

void AiBotAI::UpdateInCombatAI_Warlock()
{
    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.warlock.pDeathCoil &&
           (pVictim->CanReachWithMeleeAutoAttack(me) || pVictim->IsNonMeleeSpellCasted()) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDeathCoil))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pDeathCoil) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pShadowburn &&
           (pVictim->GetHealthPercent() < 10.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowburn))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowburn) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pSearingPain &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSearingPain))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pSearingPain) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pShadowWard &&
           (pVictim->GetClass() == CLASS_WARLOCK) &&
            CanTryToCastSpell(me, m_spells.warlock.pShadowWard))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.warlock.pShadowWard) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pImmolate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pImmolate))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pImmolate) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pConflagrate &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pConflagrate))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pConflagrate) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pCorruption &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pCorruption))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pCorruption) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pSiphonLife &&
           (me->GetHealthPercent() < 80.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pSiphonLife))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pSiphonLife) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pDrainLife &&
           (me->GetHealthPercent() < 30.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pDrainLife))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pDrainLife) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pFear)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pAdd = SelectSafeSpecAdd(pVictim))
                if (CanTryToCastSpell(pAdd, m_spells.warlock.pFear) &&   // cb:fold rotation rung, outcome probed at cast
                    DoCastSpell(pAdd, m_spells.warlock.pFear) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (pVictim->IsCaster())
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warlock.pCurseofTongues &&
                CanTryToCastSpell(pVictim, m_spells.warlock.pCurseofTongues))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warlock.pCurseofTongues) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }
        else
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warlock.pCurseofExhaustion &&
                CanTryToCastSpell(pVictim, m_spells.warlock.pCurseofExhaustion))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warlock.pCurseofExhaustion) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && me->GetDistance(pVictim) > 30.0f)
        {   // cb:fold rotation rung, outcome probed at cast
            me->GetMotionMaster()->MoveChase(pVictim, 25.0f);
        }

        if (m_spells.warlock.pShadowBolt &&
            CanTryToCastSpell(pVictim, m_spells.warlock.pShadowBolt))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warlock.pShadowBolt) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warlock.pLifeTap &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
           (me->GetHealthPercent() > 70.0f) &&
            CanTryToCastSpell(me, m_spells.warlock.pLifeTap))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.warlock.pLifeTap) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->HasSpell(AB_SPELL_SHOOT_WAND) &&
           !me->IsMoving() &&
           (me->GetPowerPercent(POWER_MANA) < 5.0f) &&
           !me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
            me->CastSpell(pVictim, AB_SPELL_SHOOT_WAND, false);   // cb:fold rotation rung, outcome probed at cast
    }
}

void AiBotAI::UpdateOutOfCombatAI_Warrior()
{
    if (m_spells.warrior.pBattleStance &&
        CanTryToCastSpell(me, m_spells.warrior.pBattleStance))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.warrior.pBattleStance) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.warrior.pBattleShout &&
       !me->HasAura(m_spells.warrior.pBattleShout->Id))
    {   // cb:fold rotation rung, outcome probed at cast
        if (CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
            DoCastSpell(me, m_spells.warrior.pBattleShout);   // cb:fold rotation rung, outcome probed at cast
        else if (m_spells.warrior.pBloodrage &&   // cb:fold rotation rung, outcome probed at cast
            (me->GetPower(POWER_RAGE) < 100) &&
            CanTryToCastSpell(me, m_spells.warrior.pBloodrage))
        {   // cb:fold rotation rung, outcome probed at cast
            DoCastSpell(me, m_spells.warrior.pBloodrage);
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.warrior.pCharge &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pCharge))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pCharge) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
}

void AiBotAI::UpdateInCombatAI_Warrior()
{
    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (pVictim->IsNonMeleeSpellCasted(false, false, true))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warrior.pPummel &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pPummel))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warrior.pPummel) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.warrior.pShieldBash &&
                IsWearingShield(me) &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldBash))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldBash) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (m_spells.warrior.pExecute &&
           (pVictim->GetHealthPercent() < 20.0f) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pExecute))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pExecute) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pOverpower &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pOverpower))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pOverpower) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pLastStand &&
            me->GetHealthPercent() < 20.0f &&
            CanTryToCastSpell(me, m_spells.warrior.pLastStand))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.warrior.pLastStand) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pConcussionBlow &&
           (pVictim->IsNonMeleeSpellCasted() || pVictim->IsMoving() || (me->GetHealthPercent() < 50.0f)) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pConcussionBlow))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pConcussionBlow) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->GetShapeshiftForm() == FORM_DEFENSIVESTANCE &&
            IsWearingShield(me))
        {   // cb:fold rotation rung, outcome probed at cast
            if (!me->GetAttackers().empty() &&
                IsPhysicalDamageClass(pVictim->GetClass()))
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.warrior.pShieldBlock &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldBlock))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.warrior.pShieldBlock) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.warrior.pShieldWall &&
                    (me->GetHealthPercent() < 40.0f) &&
                    CanTryToCastSpell(me, m_spells.warrior.pShieldWall))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.warrior.pShieldWall) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }

            if (m_spells.warrior.pShieldSlam &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pShieldSlam))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warrior.pShieldSlam) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (pVictim->IsMoving() &&
           !pVictim->HasUnitState(UNIT_STATE_ROOT) &&
           !pVictim->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warrior.pHamstring &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pHamstring))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warrior.pHamstring) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
            if (m_spells.warrior.pPiercingHowl &&
               (me->GetCombatDistance(pVictim) <= 10.0f) &&
                CanTryToCastSpell(pVictim, m_spells.warrior.pPiercingHowl))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.warrior.pPiercingHowl) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (m_spells.warrior.pRend &&
           (pVictim->GetClass() == CLASS_ROGUE) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pRend))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pRend) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pIntimidatingShout &&
           (me->GetHealthPercent() < 50.0f) &&
           (GetAttackersInRangeCount(10.0f) > 2) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntimidatingShout))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pIntimidatingShout) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pRetaliation &&
            IsMeleeDamageClass(pVictim->GetClass()) &&
           (me->GetHealthPercent() > 70.0f) &&
           ((GetAttackersInRangeCount(10.0f) > 1) || (pVictim->GetClass() == CLASS_ROGUE)) &&
            CanTryToCastSpell(me, m_spells.warrior.pRetaliation))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.warrior.pRetaliation) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if ((me->GetHealthPercent() > 60.0f) && (pVictim->GetHealthPercent() > 40.0f) &&
            (pVictim->GetClass() == CLASS_WARLOCK || pVictim->GetClass() == CLASS_PRIEST) &&
            !me->HasUnitState(UNIT_STATE_ROOT) &&
            !me->IsImmuneToMechanic(MECHANIC_FEAR))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warrior.pRecklessness &&
                CanTryToCastSpell(me, m_spells.warrior.pRecklessness))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.warrior.pRecklessness) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.warrior.pDeathWish &&
                CanTryToCastSpell(me, m_spells.warrior.pDeathWish))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.warrior.pDeathWish) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.warrior.pBerserkerRage &&
                CanTryToCastSpell(me, m_spells.warrior.pBerserkerRage))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.warrior.pBerserkerRage) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (m_spells.warrior.pMortalStrike &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pMortalStrike))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pMortalStrike) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pBloodthirst &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pBloodthirst))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pBloodthirst) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->GetHealthPercent() < 20.0f)
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warrior.pDefensiveStance &&
                CanTryToCastSpell(me, m_spells.warrior.pDefensiveStance))
            {   // cb:fold rotation rung, outcome probed at cast
                DoCastSpell(me, m_spells.warrior.pDefensiveStance);
            }
        }
        else
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.warrior.pBerserkerStance &&
               (pVictim->GetClass() != CLASS_ROGUE) &&
                CanTryToCastSpell(me, m_spells.warrior.pBerserkerStance))
            {   // cb:fold rotation rung, outcome probed at cast
                DoCastSpell(me, m_spells.warrior.pBerserkerStance);
            }
        }

        if (m_spells.warrior.pIntercept &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pIntercept))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pIntercept) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pWhirlwind &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pWhirlwind))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pWhirlwind) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.warrior.pDisarm &&
            IsMeleeWeaponClass(pVictim->GetClass()) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pDisarm))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pDisarm) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
            && !me->CanReachWithMeleeAutoAttack(pVictim))
        {   // cb:fold rotation rung, outcome probed at cast
            me->GetMotionMaster()->MoveChase(pVictim);
        }

        if (m_spells.warrior.pHeroicStrike &&
           (me->GetPower(POWER_RAGE) > 300) &&
            CanTryToCastSpell(pVictim, m_spells.warrior.pHeroicStrike))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.warrior.pHeroicStrike) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
    else // no victim
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.warrior.pBattleShout &&
            CanTryToCastSpell(me, m_spells.warrior.pBattleShout))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.warrior.pBattleShout) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
}

void AiBotAI::UpdateOutOfCombatAI_Rogue()
{
    if (m_spells.rogue.pMainHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pMainHandPoison))
    {   // cb:fold rotation rung, outcome probed at cast
        if (CastWeaponBuff(m_spells.rogue.pMainHandPoison, EQUIPMENT_SLOT_MAINHAND) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_spells.rogue.pOffHandPoison &&
        CanTryToCastSpell(me, m_spells.rogue.pOffHandPoison))
    {   // cb:fold rotation rung, outcome probed at cast
        if (CastWeaponBuff(m_spells.rogue.pOffHandPoison, EQUIPMENT_SLOT_OFFHAND) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    // Stealth is an engage tool, not a travel stance. The inherited BattleBot rotation cast it
    // on every out-of-combat tick before even checking for a victim, so open-world rogues walked
    // entire quest/vendor/trainer routes at the stealth speed penalty. Enter it only on the final
    // approach to a selected victim. Cancel an ordinary stale stealth while travelling; preserve
    // low-health stealth so Vanish can still create its intended escape window.
    Unit* const pVictim = me->GetVictim();
    bool const closeApproach = pVictim && me->IsWithinDistInMap(pVictim, 35.0f);

    if (m_spells.rogue.pStealth && me->HasAura(m_spells.rogue.pStealth->Id))
    {   // cb:fold rotation rung, outcome probed at cast
        if (!closeApproach && me->GetHealthPercent() >= 25.0f)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-combat: stale stealth cancelled for travel");
            me->RemoveAurasDueToSpellByCancel(m_spells.rogue.pStealth->Id);
        }
    }
    else if (m_spells.rogue.pStealth &&
             closeApproach &&
             CanTryToCastSpell(me, m_spells.rogue.pStealth) &&
            !me->HasAura(AURA_WARSONG_FLAG) &&
            !me->HasAura(AURA_SILVERWING_FLAG))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.rogue.pStealth) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (pVictim)
        UpdateInCombatAI_Rogue();   // cb:fold rotation rung, outcome probed at cast
}

void AiBotAI::UpdateInCombatAI_Rogue()
{
    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.rogue.pPremeditation &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pPremeditation))
            {   // cb:fold rotation rung, outcome probed at cast
                DoCastSpell(pVictim, m_spells.rogue.pPremeditation);
            }

            if (pVictim->IsCaster())
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.rogue.pGarrote &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pGarrote))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.rogue.pGarrote) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
            else
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.rogue.pAmbush &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pAmbush))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.rogue.pAmbush) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.rogue.pCheapShot &&
                    CanTryToCastSpell(pVictim, m_spells.rogue.pCheapShot))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.rogue.pCheapShot) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }
        else
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.rogue.pVanish &&
                (me->GetHealthPercent() < 10.0f))
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.rogue.pPreparation &&
                    !me->IsSpellReady(m_spells.rogue.pVanish->Id) &&
                    CanTryToCastSpell(me, m_spells.rogue.pPreparation))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.rogue.pPreparation) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (CanTryToCastSpell(me, m_spells.rogue.pVanish))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.rogue.pVanish) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        CB_HIT(me->GetGUIDLow(), "cpp-combat: vanish escape, breaking away");
                        if (me->GetMotionMaster()->MoveDistance(pVictim, 40.0f))
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                }
            }
        }

        if (me->GetComboPoints() > 4 &&
            me->GetComboTargetGuid() == pVictim->GetObjectGuid())
        {   // cb:fold rotation rung, outcome probed at cast
            SpellEntry const* pComboSpell = nullptr;
            if (pVictim->GetHealthPercent() > 65.0f &&
                m_spells.rogue.pSliceAndDice &&
                !me->HasAura(m_spells.rogue.pSliceAndDice->Id))
                pComboSpell = m_spells.rogue.pSliceAndDice;   // cb:fold rotation rung, outcome probed at cast
            else if (pVictim->GetHealthPercent() > 75.0f && m_spells.rogue.pRupture)   // cb:fold rotation rung, outcome probed at cast
                pComboSpell = m_spells.rogue.pRupture;   // cb:fold rotation rung, outcome probed at cast
            else
                pComboSpell = m_spells.rogue.pEviscerate;   // cb:fold rotation rung, outcome probed at cast

            if (pComboSpell && CanTryToCastSpell(
                    pComboSpell == m_spells.rogue.pSliceAndDice ? me : pVictim, pComboSpell))
            {   // cb:fold rotation rung, outcome probed at cast
                Unit* pComboTarget = pComboSpell == m_spells.rogue.pSliceAndDice ? me : pVictim;
                if (DoCastSpell(pComboTarget, pComboSpell) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (m_spells.rogue.pBlind)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pTarget = SelectAttackerDifferentFrom(pVictim))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.rogue.pBlind))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.rogue.pBlind) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        me->AttackStop();
                        AttackStart(pVictim);
                        return;
                    }
                }
            }
        }

        if (m_spells.rogue.pAdrenalineRush &&
           !me->GetPower(POWER_ENERGY) &&
            CanTryToCastSpell(me, m_spells.rogue.pAdrenalineRush))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.rogue.pAdrenalineRush) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (pVictim->IsNonMeleeSpellCasted())
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.rogue.pKick &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pKick))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.rogue.pKick) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.rogue.pGouge &&
                CanTryToCastSpell(pVictim, m_spells.rogue.pGouge))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pVictim, m_spells.rogue.pGouge) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (!me->HasAuraType(SPELL_AURA_MOD_STEALTH))
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.rogue.pEvasion &&
               (me->GetHealthPercent() < 80.0f) &&
               ((GetAttackersInRangeCount(10.0f) > 2) || !IsRangedDamageClass(pVictim->GetClass())) &&
                CanTryToCastSpell(me, m_spells.rogue.pEvasion))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.rogue.pEvasion) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }

            if (m_spells.rogue.pColdBlood &&
                CanTryToCastSpell(me, m_spells.rogue.pColdBlood))
            {   // cb:fold rotation rung, outcome probed at cast
                DoCastSpell(me, m_spells.rogue.pColdBlood);
            }

            if (m_spells.rogue.pBladeFlurry &&
                CanTryToCastSpell(me, m_spells.rogue.pBladeFlurry))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.rogue.pBladeFlurry) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        if (m_spells.rogue.pBackstab &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pBackstab))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.rogue.pBackstab) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.rogue.pGhostlyStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pGhostlyStrike))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.rogue.pGhostlyStrike) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.rogue.pHemorrhage &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pHemorrhage))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.rogue.pHemorrhage) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.rogue.pSinisterStrike &&
            CanTryToCastSpell(pVictim, m_spells.rogue.pSinisterStrike))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(pVictim, m_spells.rogue.pSinisterStrike) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.rogue.pSprint &&
           !me->HasUnitState(UNIT_STATE_ROOT) &&
           !me->CanReachWithMeleeAutoAttack(pVictim) &&
            CanTryToCastSpell(me, m_spells.rogue.pSprint))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.rogue.pSprint) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
}

void AiBotAI::UpdateOutOfCombatAI_Druid()
{
    BattleGround* bg = me->GetBattleGround();
    if (bg && bg->GetStatus() == STATUS_WAIT_JOIN)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.druid.pGiftoftheWild)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Player* pTarget = SelectBuffTarget(m_spells.druid.pGiftoftheWild))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.druid.pGiftoftheWild))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.druid.pGiftoftheWild) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }

        if (m_spells.druid.pThorns)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Player* pTarget = SelectBuffTarget(m_spells.druid.pThorns))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pTarget, m_spells.druid.pThorns))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pTarget, m_spells.druid.pThorns) == SPELL_CAST_OK)
                    {   // cb:fold rotation rung, outcome probed at cast
                        m_isBuffing = true;
                        return;
                    }
                }
            }
        }
    }
    else
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.druid.pMarkoftheWild && CanTryToCastSpell(me, m_spells.druid.pMarkoftheWild))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pMarkoftheWild) == SPELL_CAST_OK)
            {   // cb:fold rotation rung, outcome probed at cast
                m_isBuffing = true;
                return;
            }
        }

        if (m_spells.druid.pThorns && CanTryToCastSpell(me, m_spells.druid.pThorns))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pThorns) == SPELL_CAST_OK)
            {   // cb:fold rotation rung, outcome probed at cast
                m_isBuffing = true;
                return;
            }
        }
    }

    if (m_spells.druid.pNaturesGrasp &&
        CanTryToCastSpell(me, m_spells.druid.pNaturesGrasp))
    {   // cb:fold rotation rung, outcome probed at cast
        if (DoCastSpell(me, m_spells.druid.pNaturesGrasp) == SPELL_CAST_OK)
            return;   // cb:fold rotation rung, outcome probed at cast
    }

    if (m_isBuffing &&
       (!m_spells.druid.pMarkoftheWild ||
        !me->HasGCD(m_spells.druid.pMarkoftheWild)))
    {   // cb:fold rotation rung, outcome probed at cast
        m_isBuffing = false;
    }

    if (me->GetShapeshiftForm() == FORM_NONE)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_role == ROLE_TANK)
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.druid.pBearForm &&
                CanTryToCastSpell(me, m_spells.druid.pBearForm))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.druid.pBearForm) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }
        else if (m_role == ROLE_MELEE_DPS)
        {   // cb:fold rotation rung, outcome probed at cast
            if (m_spells.druid.pCatForm &&
                CanTryToCastSpell(me, m_spells.druid.pCatForm))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(me, m_spells.druid.pCatForm) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }
        else
        {   // cb:fold rotation rung, outcome probed at cast
            if ((me->GetPowerPercent(POWER_MANA) >  80.0f) &&
                FindAndHealInjuredAlly(80.0f))
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
    else if (me->GetShapeshiftForm() == FORM_CAT)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.druid.pProwl &&
            CanTryToCastSpell(me, m_spells.druid.pProwl) &&
            !me->HasAura(AURA_WARSONG_FLAG) &&
            !me->HasAura(AURA_SILVERWING_FLAG))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pProwl) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }

    if (me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.druid.pMoonkinForm &&
            CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        UpdateInCombatAI_Druid();
    }
    else
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.druid.pMoonkinForm &&
            me->GetShapeshiftForm() == FORM_MOONKIN)
            me->RemoveAurasDueToSpellByCancel(m_spells.druid.pMoonkinForm->Id);   // cb:fold rotation rung, outcome probed at cast

        if (m_spells.druid.pTravelForm &&
           !me->IsMounted() &&
           (!GetMountSpellId() || me->HasAura(AURA_WARSONG_FLAG) || me->HasAura(AURA_SILVERWING_FLAG)) &&
            CanTryToCastSpell(me, m_spells.druid.pTravelForm))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pTravelForm) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }
    }
}

void AiBotAI::UpdateInCombatAI_Druid()
{
    if (m_spells.druid.pTravelForm &&
        me->GetShapeshiftForm() == FORM_TRAVEL)
    {
        CB_HIT(me->GetGUIDLow(), "cpp-combat: travel form dropped for combat");
        me->RemoveAurasDueToSpellByCancel(m_spells.druid.pTravelForm->Id);
    }

    if (me->GetShapeshiftForm() == FORM_NONE)
    {   // cb:fold rotation rung, outcome probed at cast
        if (m_spells.druid.pHibernate &&
            !me->GetAttackers().empty())
        {   // cb:fold rotation rung, outcome probed at cast
            Unit* pAttacker = *me->GetAttackers().begin();
            if (CanTryToCastSpell(pAttacker, m_spells.druid.pHibernate))
            {   // cb:fold rotation rung, outcome probed at cast
                if (DoCastSpell(pAttacker, m_spells.druid.pHibernate) == SPELL_CAST_OK)
                    return;   // cb:fold rotation rung, outcome probed at cast
            }
        }

        // Heal
        if (FindAndHealInjuredAlly(80.0f))
            return;   // cb:fold rotation rung, outcome probed at cast

        // Dispels
       SpellEntry const* pDispelSpell = m_spells.druid.pAbolishPoison ?
                                         m_spells.druid.pAbolishPoison :
                                         m_spells.druid.pCurePoison;
       if (pDispelSpell)
       {   // cb:fold rotation rung, outcome probed at cast
           if (m_role == ROLE_HEALER)
           {   // cb:fold rotation rung, outcome probed at cast
               if (Unit* pFriend = SelectDispelTarget(pDispelSpell))
               {   // cb:fold rotation rung, outcome probed at cast
                   if (CanTryToCastSpell(pFriend, pDispelSpell))
                   {   // cb:fold rotation rung, outcome probed at cast
                       if (DoCastSpell(pFriend, pDispelSpell) == SPELL_CAST_OK)
                           return;   // cb:fold rotation rung, outcome probed at cast
                   }
               }
           }
           else if (IsValidDispelTarget(me, pDispelSpell) &&
                    CanTryToCastSpell(me, pDispelSpell))
           {   // cb:fold rotation rung, outcome probed at cast
               if (DoCastSpell(me, pDispelSpell) == SPELL_CAST_OK)
                   return;   // cb:fold rotation rung, outcome probed at cast
           }
       }

        if (m_spells.druid.pRemoveCurse)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pFriend = SelectDispelTarget(m_spells.druid.pRemoveCurse))
            {   // cb:fold rotation rung, outcome probed at cast
                if (CanTryToCastSpell(pFriend, m_spells.druid.pRemoveCurse))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pFriend, m_spells.druid.pRemoveCurse) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }

        if (m_spells.druid.pInnervate &&
            me->GetVictim() &&
           (me->GetHealthPercent() > 40.0f) &&
           (me->GetPowerPercent(POWER_MANA) < 10.0f) &&
            CanTryToCastSpell(me, m_spells.druid.pInnervate))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pInnervate) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_spells.druid.pMoonkinForm &&
            CanTryToCastSpell(me, m_spells.druid.pMoonkinForm))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pMoonkinForm) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        if (m_role == ROLE_MELEE_DPS || m_role == ROLE_TANK)
        {   // cb:fold rotation rung, outcome probed at cast
            if (Unit* pVictim = me->GetVictim())
            {   // cb:fold rotation rung, outcome probed at cast
                if (m_spells.druid.pBearForm &&
                    pVictim->CanReachWithMeleeAutoAttack(me) &&
                    IsPhysicalDamageClass(pVictim->GetClass()) &&
                    CanTryToCastSpell(me, m_spells.druid.pBearForm))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.druid.pBearForm) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pCatForm &&
                    CanTryToCastSpell(me, m_spells.druid.pCatForm))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.druid.pCatForm) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
            }
        }
    }
    else
    {   // cb:fold rotation rung, outcome probed at cast
        if (me->HasUnitState(UNIT_STATE_ROOT) &&
            me->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-combat: druid rooted, shifting out of form");
            me->RemoveSpellsCausingAura(SPELL_AURA_MOD_SHAPESHIFT);
        }
    }

    if (Unit* pVictim = me->GetVictim())
    {   // cb:fold rotation rung, outcome probed at cast
        ShapeshiftForm const form = me->GetShapeshiftForm();
        if (m_spells.druid.pBarkskin &&
           (form == FORM_NONE || form == FORM_MOONKIN) &&
           (me->GetHealthPercent() < 50.0f) &&
            CanTryToCastSpell(me, m_spells.druid.pBarkskin))
        {   // cb:fold rotation rung, outcome probed at cast
            if (DoCastSpell(me, m_spells.druid.pBarkskin) == SPELL_CAST_OK)
                return;   // cb:fold rotation rung, outcome probed at cast
        }

        switch (form)
        {
            case FORM_CAT:   // cb:fold rotation rung, outcome probed at cast
            {
                if (me->HasDistanceCasterMovement())
                    me->SetCasterChaseDistance(0.0f);   // cb:fold rotation rung, outcome probed at cast

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                    && !me->CanReachWithMeleeAutoAttack(pVictim))
                {   // cb:fold rotation rung, outcome probed at cast
                    me->GetMotionMaster()->MoveChase(pVictim);
                }

                if (me->HasAuraType(SPELL_AURA_MOD_STEALTH))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (m_spells.druid.pPounce &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pPounce))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(pVictim, m_spells.druid.pPounce) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                    if (m_spells.druid.pRavage &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pRavage))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(pVictim, m_spells.druid.pRavage) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                    if (m_spells.druid.pTigersFury &&
                        CanTryToCastSpell(me, m_spells.druid.pTigersFury))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(me, m_spells.druid.pTigersFury) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                    return;
                }

                if (me->GetComboPoints() > 4)
                {   // cb:fold rotation rung, outcome probed at cast
                    if (m_spells.druid.pFerociousBite &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pFerociousBite))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(pVictim, m_spells.druid.pFerociousBite) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }

                    if (m_spells.druid.pRip &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pRip))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(pVictim, m_spells.druid.pRip) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                }

                if (!me->CanReachWithMeleeAutoAttack(pVictim))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (m_spells.druid.pFaerieFireFeral &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }

                    if (m_spells.druid.pDash &&
                        pVictim->IsMoving() &&
                        CanTryToCastSpell(me, m_spells.druid.pDash))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(me, m_spells.druid.pDash) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                }

                if (m_spells.druid.pShred &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pShred))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pShred) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pRake &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pRake))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pRake) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pClaw &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pClaw))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pClaw) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                break;
            }
            case FORM_BEAR:   // cb:fold rotation rung, outcome probed at cast
            case FORM_DIREBEAR:   // cb:fold rotation rung, outcome probed at cast
            {
                if (me->HasDistanceCasterMovement())
                    me->SetCasterChaseDistance(0.0f);   // cb:fold rotation rung, outcome probed at cast

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE
                    && !me->CanReachWithMeleeAutoAttack(pVictim))
                {   // cb:fold rotation rung, outcome probed at cast
                    me->GetMotionMaster()->MoveChase(pVictim);
                }

                if (m_spells.druid.pFeralCharge &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFeralCharge))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pFeralCharge) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pBash &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pBash))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pBash) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pFrenziedRegeneration &&
                   (me->GetHealthPercent() < 30.0f) &&
                    CanTryToCastSpell(me, m_spells.druid.pFrenziedRegeneration))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(me, m_spells.druid.pFrenziedRegeneration) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pFaerieFireFeral &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFireFeral))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pFaerieFireFeral) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pDemoralizingRoar &&
                    IsMeleeDamageClass(pVictim->GetClass()) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pDemoralizingRoar))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pDemoralizingRoar) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pSwipe &&
                   ((me->GetPower(POWER_RAGE) > 500) || (GetAttackersInRangeCount(10.0f) > 1)) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pSwipe))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pSwipe) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pMaul &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pMaul))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pMaul) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }
                break;
            }
            case FORM_NONE:   // cb:fold rotation rung, outcome probed at cast
            case FORM_MOONKIN:   // cb:fold rotation rung, outcome probed at cast
            {
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE &&
                    me->GetDistance(pVictim) > 30.0f)
                {   // cb:fold rotation rung, outcome probed at cast
                    me->GetMotionMaster()->MoveChase(pVictim, 25.0f);
                }
                else if (pVictim->CanReachWithMeleeAutoAttack(me) &&
                        (pVictim->GetVictim() == me) &&
                        !me->HasUnitState(UNIT_STATE_ROOT) &&
                        (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != DISTANCING_MOTION_TYPE))
                {   // cb:fold rotation rung, outcome probed at cast
                    CB_HIT(me->GetGUIDLow(), "cpp-combat: druid caster opening range from melee");
                    if (m_spells.druid.pEntanglingRoots &&
                        CanTryToCastSpell(pVictim, m_spells.druid.pEntanglingRoots))
                    {   // cb:fold rotation rung, outcome probed at cast
                        if (DoCastSpell(pVictim, m_spells.druid.pEntanglingRoots) == SPELL_CAST_OK)
                            return;   // cb:fold rotation rung, outcome probed at cast
                    }
                    me->SetCasterChaseDistance(25.0f);
                    if (me->GetMotionMaster()->MoveDistance(pVictim, 25.0f))
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pFaerieFire &&
                   (pVictim->GetClass() == CLASS_ROGUE) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pFaerieFire))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pFaerieFire) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pInsectSwarm &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pInsectSwarm))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pInsectSwarm) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == DISTANCING_MOTION_TYPE)
                    return;   // cb:fold rotation rung, outcome probed at cast

                if (m_spells.druid.pMoonfire &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pMoonfire))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pMoonfire) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pStarfire &&
                   (pVictim->GetHealthPercent() > 50.0f) &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pStarfire))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pStarfire) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                if (m_spells.druid.pWrath &&
                    CanTryToCastSpell(pVictim, m_spells.druid.pWrath))
                {   // cb:fold rotation rung, outcome probed at cast
                    if (DoCastSpell(pVictim, m_spells.druid.pWrath) == SPELL_CAST_OK)
                        return;   // cb:fold rotation rung, outcome probed at cast
                }

                break;
            }
        }
    }
}
