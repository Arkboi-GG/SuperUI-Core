/*
 * AiBotDoctrineTeam.cpp — the TeamAuto engagement doctrine (Layer D, §2.1).
 *
 * State: an assist directive is live AND the bot is in the god-bot's group under autonomous
 * posture. This file is the ENTIRE group-fight decision. Everything that used to be smeared
 * across four sites now lives here and nowhere else:
 *   - the resolver prefix inside SelectGrindTarget      (was AiBotAIGrind.cpp 138-140)
 *   - the resolver prefix inside SelectAttackTarget     (was AiBotAICombat.cpp 545-552)
 *   - B2 mid-combat convergence                         (was AiBotAIMain.cpp 1262-1277)
 *   - B3 wait-for-anchor pull-hold + its counter        (was AiBotAIMain.cpp 915-941)
 *   - the sticky-assist memo                            (was mutable members on AiBotAI,
 *                                                        written by TeamPlay::ResolveCombatTarget)
 *
 * The sticky memo and the pull-hold counter are now INSTANCE STATE on this doctrine, so a
 * mode swap (TeamAuto -> Solo when the directive clears / the group disbands) resets them by
 * construction — the "swap == reset" semantics the whole split is built on.
 *
 * THE FLAP FIX (A/B/A/B while a follower lags): four decision points used two different
 * distance rules; this doctrine is the SINGLE authority with ONE rule — a follower COMMITS to
 * the anchor's mob and keeps it, never dropping it for merely running a few yards behind. The
 * spine's SMALL-drop and pExcept-of-anchor went away in the activation commit; with no second
 * opinion left, there is nothing to flap against.
 *
 * THE ONE-PICKER FIX (2026-07-05 — the post-kill split): the doctrine went MUTE at every kill
 * boundary, and mute defaulted to "every spine acts alone" at three seams simultaneously:
 *   (1) a kill nulls the anchor's victim AND invalidates the sticky memo (it points at the
 *       corpse), so ResolveFocus returned nullptr for every member at the same instant;
 *   (2) in combat, a nullptr from MaintainTarget deferred each follower to its own spine's
 *       SelectAttackTarget — three spines, three "nearest"s, the team splits across the adds;
 *   (3) out of combat, the B3 hold was gated on creatureEntry != 0, so the entry-0 GroupGrind
 *       task (the group's primary idle state since 2026-07-04) walked around the only follower
 *       pull-restraint that existed — three parallel SelectGrindTarget calls, the arrival
 *       fan-out reborn.
 * The fix inverts the default for followers at all three seams: NO TEAM OPINION -> HOLD or
 * KEEP, never elect. Concretely:
 *   - ResolveFocus gains a second rung: anchor victimless -> assist what is ATTACKING the
 *     anchor (GetAttackerForHelper — the same read for every follower, so the whole team
 *     converges on the SAME unit at the kill boundary);
 *   - MaintainTarget keeps a follower's own still-valid victim when there is no team focus,
 *     instead of deferring to the spine's nearest-repick;
 *   - AcquireTarget under an entry-0 task holds a follower INDEFINITELY while its anchor
 *     stands alive in range — the anchor is the only puller, period; the bounded B3 dwell
 *     remains only for objective legs and for an absent/dead anchor (the starvation valve).
 * The anchor's own path: commit to my victim until it dies — then CHAIN (2026-07-05, v3):
 * while fighting, the anchor pre-elects the NEXT target on a throttle (SelectGrindTarget with
 * the current victim excluded) and memoizes it; the instant the victim dies it adopts, in
 * order: (1) whatever is already ATTACKING me (a hostile that entered the fight preempts the
 * queue), (2) the queued next, (3) nullptr -> the legacy pick (the old path, now the fallback
 * only). No stop-scan-walk gap at the kill boundary — the team flows mob to mob. The chain is
 * SUPPRESSED whenever anyone in assist range needs recovery: the gate is aligned EXACTLY with
 * the spine's during-task eat threshold (40%), because a chain gate above the eat gate creates
 * a dead zone where nobody chains AND nobody eats — the team would stand on slow natural
 * regen. Gate closed => the spine's own eat machinery is (or is about to be) running; the
 * pause IS recovery, never a stall.
 *
 * BUILD GATE (v3): RefreshQueuedNext calls SelectGrindTarget(Unit* pExcept) — an overload the
 * Grind TU must gain (exclude pExcept in the scan; default nullptr keeps every existing call
 * site source-compatible). Do NOT build this file before AiBotAIGrind.cpp / AiBotAIMain.h
 * carry that overload.
 *
 * PARITY / TRANSCRIPTION NOTES (unchanged from the extraction):
 *   - Tap-respect is OFF for a directive-active bot (matches the shipped guard).
 *   - Sticky budget is consumed ONCE per tick.
 *   - Stalemate breaker + overpull retreat stay ON. OverpullGuard is no longer a grouped
 *     no-op (see AiBotAICombat.cpp 2026-07-05): with one-pull discipline the anchor's pull is
 *     the TEAM's pull, so the anchor now respects the group density cap before diving a camp.
 *
 * Self-contained: reads only the public AiBotAI surface (m_combatDirective, m_currentTask,
 * GetBotPlayer, IsValidAssistTarget, SelectGrindTarget, ScanApproachTarget) plus the live
 * Group it can already see. Never moves the bot, never touches m_currentTask, never emits a
 * bridge event — the spine still owns actuation and the wire (incl. GRIND_BLOCKED).
 *
 * Line endings: LF (C++ repo convention).
 */

#include "AiBotDoctrine.h"
#include "AiBotAIMain.h"
#include "AiBotCircuit.h" // [CIRCUIT] probe macros (CIRCUIT_BOARD.md)
#include "Player.h"
#include "Group.h"
#include "Creature.h"   // Map::GetCreature returns Creature* — need the complete type to upcast
#include "Map.h"        // me->GetMap()->GetCreature(guid) — same idiom the old resolver used
#include "Log.h"

#include <memory>

namespace
{

// ── Chain-queue tuning (v3) — file-local so no header churn ──
// AIBOT_CHAIN_MIN_*: the recovery suppressor. MUST equal (never exceed) the spine's
// during-task eat gate (40%) — see TeamNeedsRecovery for why a higher value dead-zones.
constexpr float  AIBOT_CHAIN_MIN_HP_PCT   = 40.0f;
constexpr float  AIBOT_CHAIN_MIN_MANA_PCT = 40.0f;
// Mid-fight next-target election cadence, in behaviour ticks (~2-4s at the usual interval):
// cheap enough to keep fresh, throttled enough that the grid scan isn't per-tick.
constexpr uint8  AIBOT_CHAIN_RESCAN_TICKS = 4;

// [CIRCUIT] Null-safe guid for probes; evaluated only inside an armed probe.
uint32 CbGuid(AiBotAI const& bot)
{
    Player* p = bot.GetBotPlayer();
    return p ? p->GetGUIDLow() : 0;
}

class AiBotDoctrineTeam final : public IEngagementDoctrine
{
public:
    // ── OOC acquisition ─────────────────────────────────────────────────────────────────
    // Resolver-first: if the anchor has a fight (live victim, its attacker, or the sticky
    // bridge), pile it. Else a FOLLOWER never self-elects: under an entry-0 task (GroupGrind)
    // it holds indefinitely while the anchor stands alive in range — the anchor is the ONLY
    // puller — and on objective legs / with an absent-dead anchor it holds the bounded B3
    // dwell. Anchor / dwell-expired fall through to the shared solo scan, so a genuinely
    // missing anchor can never starve the team.
    Unit* AcquireTarget(AiBotAI& bot) override
    {
        m_heldForTeam = false;

        Player* me = bot.GetBotPlayer();
        bool const isFollower =
            bot.m_combatDirective.IsActive() && me &&
            bot.m_combatDirective.anchorGuidLow != me->GetGUIDLow();

        if (Unit* focus = ResolveFocus(bot))
        {
            CB_HIT(CbGuid(bot), "cpp-doctrine: team, focus resolved, engaging");
            m_pullHoldTicks = 0;   // assist resolved → re-arm the hold for future engagements
            m_anchorHoldLogged = false;
            return focus;
        }

        bool const approach  = (bot.m_currentTask.type == TASK_MOVE_TO);
        bool const objective = (bot.m_currentTask.creatureEntry != 0);

        if (isFollower)
        {
            CB_HIT(CbGuid(bot), "cpp-doctrine: team, follower with no focus");
            // ONE PICKER (2026-07-05): under an entry-0 task — the GroupGrind idle state — a
            // follower NEVER self-pulls while its anchor stands alive beside it. No dwell, no
            // expiry: the anchor eats, the anchor pulls, the team fights the anchor's mob via
            // ResolveFocus. This is the seam the entry-0 wire walked around: the B3 hold below
            // was gated on creatureEntry != 0, so GroupGrind followers fell straight through to
            // three parallel SelectGrindTarget calls — the post-kill fan-out. Logged once per
            // hold episode, not per tick.
            if (!objective)
            {
                CB_HIT(CbGuid(bot), "cpp-doctrine: team, follower on idle grind leg");
                Player* anchor = FindAnchorPlayer(bot);
                if (anchor && anchor->IsAlive() &&
                    me->IsWithinDist(anchor, VISIBILITY_DISTANCE_NORMAL))
                {
                    CB_HIT(CbGuid(bot), "cpp-doctrine: team, one picker hold, anchor pulls");
                    m_heldForTeam = true;
                    if (!m_anchorHoldLogged)
                    { // cb:fold log-once latch
                        m_anchorHoldLogged = true;
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT-TEAMPLAY] %s: one-picker hold — anchor %u pulls, I assist",
                            me->GetName(), bot.m_combatDirective.anchorGuidLow);
                    }
                    return nullptr;
                }
            }

            // B3: objective leg, or the anchor is absent/dead → the bounded dwell so the anchor
            // (or the rescue machinery) gets first move, then the starvation valve opens.
            if (m_pullHoldTicks < AIBOT_ASSIST_PULL_HOLD_TICKS)
            {
                CB_HITV(CbGuid(bot), "cpp-doctrine: team, B3 pull hold, waiting for anchor", m_pullHoldTicks);
                ++m_pullHoldTicks;
                m_heldForTeam = true;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-TEAMPLAY] %s: holding pull %u/%u — waiting for anchor %u to engage",
                    me->GetName(), (uint32)m_pullHoldTicks, (uint32)AIBOT_ASSIST_PULL_HOLD_TICKS,
                    bot.m_combatDirective.anchorGuidLow);
                return nullptr;
            }
        }

        // Anchor / dwell expired → pull normally via the shared scan. Reset the counter only
        // where the hold gate doesn't apply (mirrors the old two reset sites).
        if (!isFollower || !objective)
            m_pullHoldTicks = 0;   // cb:fold hold-reset bookkeeping, hold probed at gate
        m_anchorHoldLogged = false;

        CB_HITV(CbGuid(bot), "cpp-doctrine: team, self scan fallthrough", approach ? 1 : 0);
        return approach ? bot.ScanApproachTarget() : bot.SelectGrindTarget();
    }

    // ── Pull discipline ─────────────────────────────────────────────────────────────────
    // OverpullGuard now applies a REAL group density cap (AiBotAICombat.cpp, 2026-07-05): with
    // followers pull-less, the anchor's pull is the team's pull, so the anchor holds instead of
    // diving a target buried in a 7+ cluster. (Was a grouped no-op — "let the pack absorb
    // density" — which is how trios dove 23-kobold camps.)
    bool HoldPull(AiBotAI& bot, Unit* candidate) override
    {
        return bot.OverpullGuard(candidate);
    }

    // ── In-combat maintenance (the flap fix + the kill-boundary commit) ─────────────────
    // SINGLE authority: commit to the anchor's fight (live victim, its attacker, or the sticky
    // bridge). Non-null = the spine switches to it / holds if it already matches, IGNORING
    // VISIBILITY_DISTANCE_SMALL. When there is NO team focus this tick (the kill boundary), a
    // follower KEEPS its own still-valid victim instead of deferring to the spine's
    // nearest-repick — the repick WAS the split: three spines, three "nearest"s. Only a
    // follower whose own victim is also gone defers (self-defense: the spine picks from what
    // is actually hitting it, and rung 1/2 re-converges it the next tick). The anchor always
    // defers on a dead victim — its legacy repick IS the team's next-target election.
    Unit* MaintainTarget(AiBotAI& bot, Unit* victim) override
    {
        if (Unit* focus = ResolveFocus(bot))
        {
            CB_HIT(CbGuid(bot), "cpp-doctrine: team, converging on team focus");
            return focus;
        }

        Player* me = bot.GetBotPlayer();
        bool const isFollower =
            bot.m_combatDirective.IsActive() && me &&
            bot.m_combatDirective.anchorGuidLow != me->GetGUIDLow();

        if (isFollower && victim && bot.IsValidAssistTarget(victim))
        {
            CB_HIT(CbGuid(bot), "cpp-doctrine: team, kill boundary, keeping own victim");
            return victim;   // kill-boundary commit: keep what I'm on; never elect a new mob myself
        }

        return nullptr;
    }

    // ── Shared-machinery opt-ins (parity with today's grouped bot) ──────────────────────
    bool UseStalemateBreaker() const override { return true;  }  // grouped ran HandleCombatStalemate
    bool UseOverpullRetreat()  const override { return true;  }  // grouped ran HandleOverpullRetreat (cap 6)
    bool UseTapRespect()       const override { return false; }  // directive-active bots skip the tap gate

    bool HoldingForTeam() const override { return m_heldForTeam; }

    char const* Name() const override { return "TeamAuto"; }

private:
    // The anchor read + sticky bridge. Returns the mob this follower should be on right now,
    // or nullptr ("no opinion this tick"). Called once per behaviour tick (OOC via
    // AcquireTarget, in-combat via MaintainTarget), so the sticky budget advances once per
    // tick. Rungs, in order: my-own-victim commit (anchor only) → anchor's live victim →
    // anchor's ATTACKER (the kill-boundary convergence rung, 2026-07-05) → sticky bridge →
    // nullptr.
    Unit* ResolveFocus(AiBotAI& bot)
    {
        CombatDirective const& dir = bot.m_combatDirective;
        if (!dir.IsActive())
        {
            CB_HIT(CbGuid(bot), "cpp-doctrine: team, no focus, directive inactive");
            return nullptr;
        }

        Player* me = bot.GetBotPlayer();
        if (!me || !me->IsInWorld())
        {
            CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-doctrine: team, no focus, bot not in world");
            return nullptr;
        }

        if (dir.mode != COMBAT_MODE_ASSIST)
        {
            CB_HITV(me->GetGUIDLow(), "cpp-doctrine: team, no focus, mode not assist", dir.mode);
            return nullptr;
        }

        // I AM the anchor. The whole team mirrors my GetVictim(), so commit to my OWN current
        // victim until it dies — ignore the legacy re-pick — so the team locks one mob at a
        // time. Only when my victim is gone do I return nullptr, deferring to the legacy pick
        // (SelectAttackTarget / SelectGrindTarget) for the next target, which becomes the new
        // team focus on the following tick. Stalemate/overpull breakers still fire
        // (UseX()==true), so a stuck/unreachable victim can't glue me forever.
        if (dir.anchorGuidLow == me->GetGUIDLow())
        {
            CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, i am the anchor");
            Unit* mine = (me->GetVictim() && bot.IsValidAssistTarget(me->GetVictim()))
                       ? me->GetVictim() : nullptr;

            if (mine)
            {
                CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, anchor commits to own victim");
                // Fighting: keep the commitment AND pre-elect the next target on a throttle
                // (the chain queue). The election happens DURING the fight so the kill boundary
                // is a handoff, not a stop-scan-walk.
                RefreshQueuedNext(bot, me, mine);
                TraceAnchorFocus(me, mine, "self, committing until dead");
                return mine;
            }

            // Victim just died / gone → CHAIN. Preemption order:
            //   (1) something already attacking me — a hostile that entered the fight outranks
            //       the queue (Nico: "unless another hostile enters");
            //   (2) the queued next — validated fresh (alive, hostile, in range), consumed once;
            //   (3) nullptr → the legacy pick (the pre-v3 path, now the fallback only).
            // All of it suppressed while anyone in assist range needs recovery — the pause IS
            // the eat window, by construction (gate == the spine's during-task eat threshold).
            if (!TeamNeedsRecovery(me))
            {
                CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, chain window open");
                if (Unit* pAggro = me->GetAttackerForHelper())
                { // cb:fold candidate gate, chain probe below
                    if (bot.IsValidAssistTarget(pAggro) &&
                        me->IsWithinDist(pAggro, VISIBILITY_DISTANCE_NORMAL))
                    {
                        CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, chain, attacker preempts queue");
                        m_queuedNextGuid.Clear();   // the fight found us; the queue re-elects next fight
                        TraceAnchorFocus(me, pAggro, "chain: attacker preempts");
                        return pAggro;
                    }
                }

                if (!m_queuedNextGuid.IsEmpty())
                { // cb:fold queue gate, adoption probe below
                    if (Creature* pNext = me->GetMap()->GetCreature(m_queuedNextGuid))
                    { // cb:fold queued lookup, adoption probe below
                        if (bot.IsValidAssistTarget(pNext) &&
                            me->IsWithinDist(pNext, VISIBILITY_DISTANCE_NORMAL))
                        {
                            CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, chain, adopting queued next");
                            m_queuedNextGuid.Clear();   // consumed
                            TraceAnchorFocus(me, pNext, "chain: queued next");
                            return pNext;
                        }
                    }
                    m_queuedNextGuid.Clear();   // stale (despawned / grey / out of range) — discard
                }
            }

            CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, anchor defers to legacy pick");
            TraceAnchorFocus(me, nullptr, "self, committing until dead");
            return nullptr;   // recovery window, or nothing queued → legacy re-picks when ready
        }

        Player* anchor = FindAnchorPlayer(bot);
        if (!anchor)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, anchor missing, standing down");
            ClearSticky();
            return nullptr;   // not grouped this tick, or anchor offline / not in our group
        }

        Unit* pVictim = anchor->GetVictim();

        // Rung 1: a live, valid, in-range anchor victim is authoritative: adopt + remember +
        // reset the sticky budget.
        if (pVictim && bot.IsValidAssistTarget(pVictim) &&
            me->IsWithinDist(pVictim, VISIBILITY_DISTANCE_NORMAL))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, rung 1, adopting anchor victim");
            m_lastAssistedVictimGuid = pVictim->GetObjectGuid();
            m_assistStickyTicks = 0;
            return pVictim;
        }

        // Rung 2 (2026-07-05, the kill-boundary convergence): the anchor is victimless — the
        // ordinary state for the instant after a kill — but something may be ATTACKING him
        // (the add that jumped in during the gap). Assist THAT. GetAttackerForHelper is the
        // same deterministic read for every follower, so the whole team lands on the SAME
        // unit, where the old path (sticky pointing at the fresh corpse → invalid → mute →
        // three spines each picking their own nearest) split it. Also covers the anchor
        // retargeting mid-fight — the FUTURE SEAM this file used to defer, bought with an
        // attacker read instead of a HostileRefManager walk.
        if (anchor->IsAlive())
        { // cb:fold candidate gate, rung 2 probe below
            if (Unit* pAggro = anchor->GetAttackerForHelper())
            { // cb:fold candidate gate, rung 2 probe below
                if (bot.IsValidAssistTarget(pAggro) &&
                    me->IsWithinDist(pAggro, VISIBILITY_DISTANCE_NORMAL))
                {
                    CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, rung 2, assisting anchor attacker");
                    m_lastAssistedVictimGuid = pAggro->GetObjectGuid();
                    m_assistStickyTicks = 0;
                    return pAggro;
                }
            }
        }

        // Rung 3: no live anchor fight this tick — bridge the anchor's between-kill gap with
        // the last mob we assisted, through the SAME validity gate (a mob the anchor just
        // killed self-clears), bounded by the tick budget so a genuinely-disengaged anchor
        // can't glue us to a stale mob forever.
        if (!m_lastAssistedVictimGuid.IsEmpty() && m_assistStickyTicks < AIBOT_ASSIST_STICKY_MAX_TICKS)
        { // cb:fold sticky gate, hold probe below
            if (Creature* pSticky = me->GetMap()->GetCreature(m_lastAssistedVictimGuid))
            { // cb:fold sticky lookup, hold probe below
                if (bot.IsValidAssistTarget(pSticky) &&
                    me->IsWithinDist(pSticky, VISIBILITY_DISTANCE_NORMAL))
                {
                    CB_HITV(me->GetGUIDLow(), "cpp-doctrine: team, rung 3, sticky bridge hold", m_assistStickyTicks);
                    ++m_assistStickyTicks;
                    return pSticky;
                }
            }
        }

        ClearSticky();
        CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, follower, no team focus this tick");
        return nullptr;
    }

    // ── Chain queue (v3) ─────────────────────────────────────────────────────────────
    // Throttled pre-election of the anchor's NEXT target while it fights the current one.
    // Uses the pExcept overload of SelectGrindTarget (BUILD GATE — see header) so the scan
    // can never hand back the mob we're already killing. Skipped on approach legs (the spine
    // scans those with ScanApproachTarget); an entry-carrying grind task chains within its
    // entry by SelectGrindTarget's own task rules, an entry-0 grind chains nearest-valid —
    // both exactly what "no downtime between kills" means for that task.
    void RefreshQueuedNext(AiBotAI& bot, Player* me, Unit* current)
    {
        if (bot.m_currentTask.type == TASK_MOVE_TO)
        {
            CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, chain skip, approach leg");
            return;   // approach legs don't chain — arrival owns the first pull
        }

        if (++m_chainRescanTicks < AIBOT_CHAIN_RESCAN_TICKS)
            return;   // cb:fold rescan throttle
        m_chainRescanTicks = 0;

        Unit* next = bot.SelectGrindTarget(current);   // pExcept overload (BUILD GATE)
        if (next && next != current && bot.IsValidAssistTarget(next) &&
            me->IsWithinDist(next, VISIBILITY_DISTANCE_NORMAL))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, chain, next target elected");
            m_queuedNextGuid = next->GetObjectGuid();
        }
        // A miss deliberately KEEPS the previous memo: the queued mob is re-validated at
        // consumption anyway, and a transient scan miss (LOS flicker) shouldn't drop a good
        // election.
    }

    // The chain suppressor: anyone in assist range at/below the spine's during-task eat
    // threshold (40% HP; 40% mana for mana users) means the next window is an EAT window,
    // not a pull window. MUST NOT exceed the spine's eat gate — a higher chain gate opens a
    // dead zone where nobody chains and nobody eats (standing on natural regen).
    bool TeamNeedsRecovery(Player* me) const
    {
        if (PlayerNeedsRecovery(me))
        {
            CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, recovery needed, self low");
            return true;
        }

        Group* pGroup = me->GetGroup();
        if (!pGroup)
            return false;   // cb:fold ungrouped, pure read

        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (!pMember || pMember == me || !pMember->IsAlive())
                continue;   // cb:fold member scan gate
            if (!me->IsWithinDist(pMember, VISIBILITY_DISTANCE_NORMAL))
                continue;   // cb:fold member scan, out of range
            if (PlayerNeedsRecovery(pMember))
            {
                CB_HIT(me->GetGUIDLow(), "cpp-doctrine: team, recovery needed, member low");
                return true;
            }
        }
        return false;
    }

    static bool PlayerNeedsRecovery(Player* p)
    {
        if (p->GetHealthPercent() < AIBOT_CHAIN_MIN_HP_PCT)
            return true;   // cb:fold pure threshold read, caller probes outcome
        if (p->GetPowerType() == POWER_MANA &&
            p->GetPowerPercent(POWER_MANA) < AIBOT_CHAIN_MIN_MANA_PCT)
            return true;   // cb:fold pure threshold read, caller probes outcome
        return false;
    }

    // The anchor focus tracer — logs only when the committed focus CHANGES (never per-tick),
    // now shared by the commit / chain / defer paths so the chain cadence is directly visible.
    void TraceAnchorFocus(Player* me, Unit* focus, char const* how)
    {
        ObjectGuid g = focus ? focus->GetObjectGuid() : ObjectGuid();
        if (g == m_lastAnchorSelfVictim)
            return;   // cb:fold log throttle, focus unchanged
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-DOCTRINE] %s: ANCHOR focus -> %s (%s)",
            me->GetName(), focus ? focus->GetName() : "(none)", how);
        m_lastAnchorSelfVictim = g;
    }

    // The anchor lookup (the same Group-walk idiom SelectAttackTarget uses), shared by
    // ResolveFocus and the one-picker hold in AcquireTarget.
    Player* FindAnchorPlayer(AiBotAI& bot)
    {
        Player* me = bot.GetBotPlayer();
        if (!me || !me->IsInWorld())
            return nullptr;   // cb:fold anchor lookup helper, callers probe outcome

        Group* pGroup = me->GetGroup();
        if (!pGroup)
            return nullptr;   // cb:fold anchor lookup helper, callers probe outcome

        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (pMember && pMember->GetGUIDLow() == bot.m_combatDirective.anchorGuidLow)
                return pMember;   // cb:fold anchor lookup hit, callers probe outcome
        }
        return nullptr;
    }

    void ClearSticky()
    {
        m_lastAssistedVictimGuid.Clear();
        m_assistStickyTicks = 0;
    }

    // Instance-owned transient state (was on AiBotAI). Swap to Solo destroys this doctrine and
    // the state with it — no reset checklist needed.
    ObjectGuid m_lastAssistedVictimGuid;
    ObjectGuid m_lastAnchorSelfVictim;    // anchor-self focus tracer — logs only when it changes
    ObjectGuid m_queuedNextGuid;          // v3 chain queue: the pre-elected next target (anchor only)
    uint8      m_assistStickyTicks = 0;   // budget consumed while bridging a between-kill gap
    uint8      m_pullHoldTicks     = 0;   // B3 wait-for-anchor dwell (objective legs / absent anchor)
    uint8      m_chainRescanTicks  = 0;   // throttle for the mid-fight next-target election
    bool       m_heldForTeam       = false;
    bool       m_anchorHoldLogged  = false;   // one-picker hold: log once per episode, not per tick
};

} // anonymous namespace

// Factory hook consumed by MakeDoctrine() in AiBotDoctrine.cpp (activation commit).
std::unique_ptr<IEngagementDoctrine> MakeTeamDoctrine()
{
    return std::unique_ptr<IEngagementDoctrine>(new AiBotDoctrineTeam());
}