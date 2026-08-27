/*
 * AiBotDoctrinePlayerParty.cpp — the PlayerParty (escort) engagement doctrine (2026-07-07).
 *
 * State: the bot's group contains a REAL player (a non-bot session). A human invited this
 * bot to their party — the human IS the coordinator. Auto-selected by ResolveDoctrine
 * (AiBotDoctrine.cpp) whenever FindPartyBoss() resolves, ranked ABOVE the directive-driven
 * TeamAuto so a stale C# stamp can never out-vote a live human; reverts the tick after the
 * bot leaves the group (kick / disband / logout), swap == reset as always.
 *
 * The spec (Nico, 2026-07-07): "They would attack my targets, and only attack not-my-target
 * if I'm not attacking anything but being attacked; healers keep their rotation." Verbatim
 * the assist-priority ladder, so this doctrine is deliberately the SMALL sibling of TeamAuto:
 * the same resolver rungs with the real player as the anchor, MINUS every self-election path.
 *
 *   AcquireTarget  → ResolvePartyFocus or NOTHING. A companion NEVER falls through to
 *                    SelectGrindTarget / ScanApproachTarget — zero initiation, ever. (The
 *                    escort spine hook in UpdateAI also returns before the task-dispatch
 *                    blocks, so the grind machinery is doubly unreachable; this doctrine
 *                    holds the invariant even if that ordering ever changes.)
 *   MaintainTarget → the focus if one resolves (converge/switch, exactly the TeamAuto
 *                    seam); else KEEP my own still-valid victim (finish the mob that jumped
 *                    me); else nullptr → the spine's SelectAttackTarget, which picks only
 *                    from attackers of me + my group = pure defense, never a fresh pull.
 *
 * ResolvePartyFocus rungs, in order:
 *   1. the boss's live victim            — "attack my targets";
 *   2. the boss's attacker               — "I'm not attacking, but I'm being attacked"
 *                                          (GetAttackerForHelper: one deterministic read,
 *                                          every companion converges on the SAME unit);
 *   3. any party member's attacker       — defend the rest of the party (the healer being
 *                                          eaten while the boss reads a quest text);
 *   4. the sticky bridge                 — the TeamAuto between-kill memo, same budget, so
 *                                          companions don't flicker off a mob the instant
 *                                          the boss's GetVictim() nulls mid-chain-pull.
 *
 * Rotations are untouched: this doctrine only decides WHO to hit; healers keep healing via
 * their class AI exactly as everywhere else.
 *
 * Shared-machinery stances (the deliberate differences from TeamAuto):
 *   UseOverpullRetreat = FALSE — a companion must NEVER AttackStop and flee 30yd out of the
 *     player's fight because the pull ran deep; the HUMAN owns pull depth, and abandoning
 *     his fight mid-heal is the worst possible move. Defense-to-the-end beside a real
 *     player is correct — he can heal, peel, or run, and the C# death machinery still
 *     recovers a wipe.
 *   UseStalemateBreaker = TRUE — an unreachable-mob deadlock is a navmesh problem, not a
 *     tactics problem; the escape machinery stays.
 *   UseTapRespect = TRUE — the boss's tap is group-shared (the spine's tapperInMyGroup
 *     check passes it); a STRANGER's tapped mob is still off-limits — companions don't
 *     steal mobs from other real players.
 *
 * Self-contained: reads only the public AiBotAI surface (GetBotPlayer, FindPartyBoss,
 * IsValidAssistTarget) plus the live Group. Never moves the bot, never touches
 * m_currentTask, never emits a bridge event — the spine owns actuation and the wire
 * (the escort follow/engage hook lives in AiBotAIMain.cpp).
 *
 * Line endings: LF (C++ repo convention).
 */

#include "AiBotDoctrine.h"
#include "AiBotAIMain.h"
#include "AiBotCircuit.h" // [CIRCUIT] probe macros (CIRCUIT_BOARD.md)
#include "Player.h"
#include "WorldSession.h"   // [MULTI-HUMAN] GetSession()->GetBot() — the real-player test in rungs 1-2
#include "Group.h"
#include "Creature.h"   // Map::GetCreature returns Creature* — need the complete type (sticky lookup)
#include "Map.h"
#include "Log.h"
#include "GridNotifiers.h"       // rung 3b mob-side scan (2026-07-08)
#include "GridNotifiersImpl.h"
#include "CellImpl.h"

#include <memory>
#include <list>

namespace
{

// [CIRCUIT] Null-safe guid for probes; evaluated only inside an armed probe.
uint32 CbGuid(AiBotAI const& bot)
{
    Player* p = bot.GetBotPlayer();
    return p ? p->GetGUIDLow() : 0;
}

class AiBotDoctrinePlayerParty final : public IEngagementDoctrine
{
public:
    // ── OOC acquisition ─────────────────────────────────────────────────────────────────
    // The party focus or NOTHING. No dwell, no expiry, no starvation valve — a companion
    // with no team fight to join stands at the boss's shoulder (the escort hook follows);
    // it never seeds a fight. m_heldForTeam stays true so, should the grind dispatch ever
    // be reached with this doctrine live, a null here reads as a deliberate hold — never a
    // freeze tick, never a GRIND_BLOCKED handback.
    Unit* AcquireTarget(AiBotAI& bot) override
    {
        m_heldForTeam = true;
        return ResolvePartyFocus(bot);
    }

    // ── Pull discipline ─────────────────────────────────────────────────────────────────
    // Never veto. The only pulls this doctrine can produce are the boss's own fights
    // (rung 1/2/3 targets are already engaged with the party) — density is the human's
    // call, and holding a companion out of his 6-mob pull is exactly wrong.
    bool HoldPull(AiBotAI& bot, Unit* candidate) override
    {
        (void)bot; (void)candidate;
        return false;
    }

    // ── In-combat maintenance ───────────────────────────────────────────────────────────
    // Converge on the party focus every tick (the boss retargets → the whole escort
    // switches, the TeamAuto flap fix inherited whole). No focus → keep my own still-valid
    // victim (the mob that jumped ME — finish it). Neither → nullptr, and the spine's
    // SelectAttackTarget picks from attackers of me + my group: defense, never initiation.
    Unit* MaintainTarget(AiBotAI& bot, Unit* victim) override
    {
        if (Unit* focus = ResolvePartyFocus(bot))
        {
            CB_HIT(CbGuid(bot), "cpp-doctrine: pparty, converging on party focus");
            return focus;
        }

        if (victim && bot.IsValidAssistTarget(victim))
        {
            CB_HIT(CbGuid(bot), "cpp-doctrine: pparty, keeping own valid victim");
            return victim;
        }

        return nullptr;
    }

    // ── Shared-machinery stances (see the file header for the reasoning) ────────────────
    bool UseStalemateBreaker() const override { return true;  }
    bool UseOverpullRetreat()  const override { return false; }  // never flee the human's fight
    bool UseTapRespect()       const override { return true;  }  // party taps pass; strangers' don't

    bool HoldingForTeam() const override { return m_heldForTeam; }

    char const* Name() const override { return "PlayerParty"; }

private:
    // The party focus ladder — the mob every companion should be on right now, or nullptr
    // ("nothing to fight; follow"). Called once per behaviour tick (OOC via AcquireTarget,
    // in-combat via MaintainTarget), so the sticky budget advances once per tick, same as
    // TeamAuto.
    Unit* ResolvePartyFocus(AiBotAI& bot)
    {
        Player* me = bot.GetBotPlayer();
        if (!me || !me->IsInWorld())
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-doctrine: pparty, no focus, bot not in world");
            return nullptr;
        }

        Player* boss = bot.FindPartyBoss();
        if (!boss || !boss->IsInWorld())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-doctrine: pparty, boss gone, standing down");
            ClearSticky();
            return nullptr;   // human left / logged — ResolveDoctrine flips us back next tick
        }

        // [MULTI-HUMAN] (2026-07-16) Rungs 1-2 run over EVERY real player, the bot's
        // ASSIGNED human first — with two humans in the party, the OTHER human's fights
        // used to be invisible to these rungs (only rung 3/3b could catch them, and only
        // once a mob was already beating on a player), which is exactly the "bots stand
        // there for a hot minute" Deadmines report. FindEscortBoss front-loads the human
        // this bot escorts, so "attack my targets" now means YOUR human's targets first,
        // then any human's — deterministic per bot, converged per escort half.
        Player* humans[8];
        int humanCount = 0;
        {
            if (Player* pAssigned = bot.FindEscortBoss())
                humans[humanCount++] = pAssigned;   // cb:fold roster gather detail
            if (Group* pGroup = me->GetGroup())
            { // cb:fold roster gather, rung probes carry the outcome
                for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr && humanCount < 8; itr = itr->next())
                {
                    Player* pMember = itr->getSource();
                    if (!pMember || pMember == me || !pMember->IsInWorld())
                        continue;   // cb:fold roster gather scan
                    WorldSession* pSess = pMember->GetSession();
                    if (!pSess || pSess->GetBot())
                        continue;   // cb:fold roster gather scan, a bot not a human
                    if (humanCount > 0 && pMember == humans[0])
                        continue;   // cb:fold roster gather scan, assigned human already front-loaded
                    humans[humanCount++] = pMember;
                }
            }
        }

        for (int h = 0; h < humanCount; ++h)
        {
            Player* pHuman = humans[h];
            if (!pHuman->IsAlive())
                continue;   // cb:fold per-human scan, dead human

            // Rung 1: a human's own fight is authoritative — "attack my targets".
            if (Unit* pVictim = pHuman->GetVictim())
            { // cb:fold candidate gate, rung 1 probe below
                if (bot.IsValidAssistTarget(pVictim) &&
                    me->IsWithinDist(pVictim, VISIBILITY_DISTANCE_NORMAL))
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-doctrine: pparty, rung 1, attacking human victim");
                    m_lastAssistedVictimGuid = pVictim->GetObjectGuid();
                    m_assistStickyTicks = 0;
                    TraceFocus(me, pVictim, h == 0 ? "my human's victim" : "human victim");
                    return pVictim;
                }
            }

            // Rung 2: the human is not attacking, but something is attacking HIM —
            // "only attack not-my-target if I'm not attacking anything but being attacked".
            // GetAttackerForHelper is the same deterministic read for every companion, so
            // each escort half converges on the SAME unit.
            if (Unit* pAggro = pHuman->GetAttackerForHelper())
            { // cb:fold candidate gate, rung 2 probe below
                if (bot.IsValidAssistTarget(pAggro) &&
                    me->IsWithinDist(pAggro, VISIBILITY_DISTANCE_NORMAL))
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-doctrine: pparty, rung 2, defending human under attack");
                    m_lastAssistedVictimGuid = pAggro->GetObjectGuid();
                    m_assistStickyTicks = 0;
                    TraceFocus(me, pAggro, h == 0 ? "my human's attacker" : "human attacker");
                    return pAggro;
                }
            }
        }

        // Rung 3: defend the rest of the party — a mob eating the priest while the boss
        // reads quest text. First member-under-attack in group order (deterministic enough;
        // the whole escort walks the same list).
        if (Group* pGroup = me->GetGroup())
        { // cb:fold group walk, rung 3 probe below
            for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* pMember = itr->getSource();
                if (!pMember || pMember == me || pMember == boss || !pMember->IsAlive())
                    continue;   // cb:fold member scan gate
                if (!me->IsWithinDist(pMember, VISIBILITY_DISTANCE_NORMAL))
                    continue;   // cb:fold member scan, out of range
                if (Unit* pAggro = pMember->GetAttackerForHelper())
                { // cb:fold candidate gate, rung 3 probe below
                    if (bot.IsValidAssistTarget(pAggro) &&
                        me->IsWithinDist(pAggro, VISIBILITY_DISTANCE_NORMAL))
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-doctrine: pparty, rung 3, defending party member");
                        m_lastAssistedVictimGuid = pAggro->GetObjectGuid();
                        m_assistStickyTicks = 0;
                        TraceFocus(me, pAggro, "member attacker");
                        return pAggro;
                    }
                }
            }
        }

        // Rung 3b (2026-07-08, the standing-escort fix): the MOB-SIDE read — any hostile
        // creature near me whose victim IS a party member. Rungs 1-3 read the fight off the
        // PLAYER side (GetVictim / GetAttackerForHelper), which TeamAuto only ever proved
        // against BOT anchors; this rung reads it off the CREATURE side (c->GetVictim() ∈
        // group), which cannot lie — if a mob is beating on anyone in the party, this sees
        // it regardless of how the core books a real player's victim/attacker state. Same
        // acquisition shape as vmangos's own PartyBotAI. Nearest wins; boss-targeting mobs
        // outrank member-targeting ones so the spec's priority survives inside the rung.
        {
            std::list<Unit*> nearby;
            MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck u_check(me, me, 40.0f);
            MaNGOS::UnitListSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(nearby, u_check);
            Cell::VisitAllObjects(me, searcher, 40.0f);

            Unit* bestBoss = nullptr;   float bestBossD = 99999.0f;
            Unit* bestMemb = nullptr;   float bestMembD = 99999.0f;
            Group* pGroup = me->GetGroup();
            for (Unit* u : nearby)
            {
                if (!u->IsCreature())
                    continue;   // cb:fold mob-side scan gate
                Unit* uVictim = u->GetVictim();
                if (!uVictim)
                    continue;   // cb:fold mob-side scan gate, idle mob
                // [PET-FIX] (2026-07-16) Resolve pets/minions to their owning player: a mob
                // beating on the warlock's voidwalker or the hunter's cat IS a party fight,
                // but uVictim->IsPlayer() used to exclude it — the Hogger report (mob tagged
                // on a real player's minion, escort stood watching) lands exactly here.
                Player* pVictimPlayer = uVictim->GetCharmerOrOwnerPlayerOrPlayerItself();
                if (!pVictimPlayer)
                    continue;   // cb:fold mob-side scan gate, not a player fight
                bool const onBoss = (pVictimPlayer == boss);
                bool const onMember = onBoss ||
                    (pVictimPlayer == me) ||
                    (pGroup && pGroup->IsMember(pVictimPlayer->GetObjectGuid()));
                if (!onMember)
                    continue;   // cb:fold mob-side scan gate, not our party
                if (!bot.IsValidAssistTarget(u))
                    continue;   // cb:fold mob-side scan gate, invalid target
                float const d = me->GetDistance(u);
                if (onBoss)      { if (d < bestBossD) { bestBossD = d; bestBoss = u; } }   // cb:fold nearest-pick bookkeeping
                else             { if (d < bestMembD) { bestMembD = d; bestMemb = u; } }   // cb:fold nearest-pick bookkeeping
            }
            if (Unit* pick = bestBoss ? bestBoss : bestMemb)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-doctrine: pparty, rung 3b, mob-side scan pick");
                m_lastAssistedVictimGuid = pick->GetObjectGuid();
                m_assistStickyTicks = 0;
                TraceFocus(me, pick, "mob-side scan");
                return pick;
            }
        }

        // Rung 4: the sticky bridge — the boss's GetVictim() nulls for a beat between
        // swings / kills; keep the escort on the last assisted mob through the gap, same
        // validity gate + budget as TeamAuto (a mob the boss just killed self-clears).
        if (!m_lastAssistedVictimGuid.IsEmpty() && m_assistStickyTicks < AIBOT_ASSIST_STICKY_MAX_TICKS)
        { // cb:fold sticky gate, hold probe below
            if (Creature* pSticky = me->GetMap()->GetCreature(m_lastAssistedVictimGuid))
            { // cb:fold sticky lookup, hold probe below
                if (bot.IsValidAssistTarget(pSticky) &&
                    me->IsWithinDist(pSticky, VISIBILITY_DISTANCE_NORMAL))
                {
                    CB_HITV(me->GetGUIDLow(), "cpp-doctrine: pparty, rung 4, sticky bridge hold", m_assistStickyTicks);
                    ++m_assistStickyTicks;
                    return pSticky;
                }
            }
        }

        ClearSticky();

        // [DIAG] Null-focus tracer (2026-07-08, throttled ~1/10s per bot): when the escort
        // stands while the party fights, ONE of these fields names the failing gate — a live
        // bossVictim with v_valid=0 indicts IsValidHostileTarget; victim=none + attacker=none
        // while mobs visibly swing indicts the player-side reads (and rung 3b above should
        // have caught it mob-side). Cheap; remove after the shakedown.
        if (++m_diagTick >= 10)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-doctrine: pparty, no focus, diag window");
            m_diagTick = 0;
            Unit* bv = boss->IsAlive() ? boss->GetVictim() : nullptr;
            Unit* ba = boss->IsAlive() ? boss->GetAttackerForHelper() : nullptr;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-PARTY-DIAG] %s: no focus — boss=%s alive=%u victim=%s v_valid=%u attacker=%s a_valid=%u",
                me->GetName(), boss->GetName(), boss->IsAlive() ? 1u : 0u,
                bv ? bv->GetName() : "(none)", bv ? (bot.IsValidAssistTarget(bv) ? 1u : 0u) : 0u,
                ba ? ba->GetName() : "(none)", ba ? (bot.IsValidAssistTarget(ba) ? 1u : 0u) : 0u);
        }
        return nullptr;
    }

    // Log only when the resolved focus CHANGES (never per-tick) — the TeamAuto tracer pattern.
    void TraceFocus(Player* me, Unit* focus, char const* how)
    {
        ObjectGuid const g = focus ? focus->GetObjectGuid() : ObjectGuid();
        if (g == m_lastFocusGuid)
            return;   // cb:fold log throttle, focus unchanged
        m_lastFocusGuid = g;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PARTY] %s: focus -> %s (%s)",
            me->GetName(), focus ? focus->GetName() : "(none)", how);
    }

    void ClearSticky()
    {
        m_lastAssistedVictimGuid.Clear();
        m_assistStickyTicks = 0;
    }

    // Instance-owned transient state — a doctrine swap (human leaves) destroys it with the
    // instance, no reset checklist needed (the same "swap == reset" the split is built on).
    ObjectGuid m_lastAssistedVictimGuid;
    ObjectGuid m_lastFocusGuid;           // focus-change tracer memo (log on change only)
    uint8      m_assistStickyTicks = 0;
    uint8      m_diagTick          = 0;   // null-focus DIAG throttle (~1 line / 10 ticks)
    bool       m_heldForTeam       = true;
};

} // anonymous namespace

// Factory hook consumed by MakeDoctrine() in AiBotDoctrine.cpp.
std::unique_ptr<IEngagementDoctrine> MakePlayerPartyDoctrine()
{
    return std::unique_ptr<IEngagementDoctrine>(new AiBotDoctrinePlayerParty());
}