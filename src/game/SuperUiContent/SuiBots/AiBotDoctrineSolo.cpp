/*
 * AiBotDoctrineSolo.cpp — the Solo engagement doctrine (Layer D, §2.1).
 *
 * State: ungrouped, or grouped with no active directive. This is TODAY'S solo behaviour,
 * transcribed — priority scan, overpull density veto, tap-respect, stalemate + retreat
 * enabled. It is a THIN WRAPPER: every decision forwards to an existing AiBotAI method that
 * already ships and is verified; the doctrine adds no new logic and holds no transient state
 * (Solo has none — the B3 pull-hold counter and the sticky memo are TeamAuto's concern).
 *
 * PARITY NOTE (M1 gate): this TU is purely ADDITIVE. Nothing constructs or calls a doctrine
 * yet — MakeDoctrine()/the four dispatch sites land in the later activation commit — so
 * dropping this file in (and its CMake line) changes zero behaviour. It compiles alongside
 * the shipped AI and waits. That is deliberate: the doctrine substrate goes in first and
 * cold, the dispatch rewires to it second, so the behaviour-changing step is isolated and
 * reviewable on its own.
 *
 * Includes: AiBotDoctrine.h (the interface) + AiBotAIMain.h (the AiBotAI surface — every
 * method called here is public, verified against the single `public:` section of that header).
 * Line endings: LF (C++ repo convention).
 */

#include "AiBotDoctrine.h"
#include "AiBotAIMain.h"

#include <memory>

namespace
{

class AiBotDoctrineSolo final : public IEngagementDoctrine
{
public:
    // OOC acquisition (§2.4). Two dispatch sites, one method, distinguished by the committed
    // task kind — exactly the doc's "grind / enriched-MOVE_TO approach" split:
    //   TASK_MOVE_TO -> the approach scan (engage any valid in-log quest mob on the way in).
    //   TASK_GRIND   -> the solo priority scan.
    // Both are the SHIPPED calls verbatim. SelectGrindTarget/ScanApproachTarget stay in
    // AiBotAIGrind.cpp as shared scan machinery; the doctrine only chooses which to run.
    // (For a solo bot m_combatDirective is inactive, so the resolver prefix inside
    // SelectGrindTarget is a proven no-op here — Solo == today, bit-for-bit.)
    Unit* AcquireTarget(AiBotAI& bot) override
    {
        if (bot.m_currentTask.type == TASK_MOVE_TO)
            return bot.ScanApproachTarget();
        return bot.SelectGrindTarget();
    }

    // Pull discipline (§2.4): the OverpullGuard density veto on an acquired candidate.
    // OverpullGuard self-returns false when grouped, so this is the SOLO cap by construction
    // (a Solo-doctrine bot is never grouped-with-directive anyway).
    //
    // What is NOT here, on purpose: the freeze self-unstick ESCALATION that, after
    // AIBOT_GRIND_FREEZE_DWELL frozen ticks, pulls anyway (bypassing this veto). That path
    // counts m_grindFreezeStreak, may emit GRIND_BLOCKED on the wire, and drives
    // DoGrindPatrol/AttackStart — task bookkeeping + wire + actuation, which the doctrine
    // explicitly does not own (§2.3). It stays at the UpdateAI dispatch site as shared
    // mechanism; the doctrine supplies only the per-tick veto. (Extraction decision D3 —
    // the doc lists the escalation under Solo HoldPull; keeping it at the dispatch is the
    // parity-safe reading, since moving the GRIND_BLOCKED emit into a doctrine would drag
    // the bridge across the ownership line the doc draws.)
    bool HoldPull(AiBotAI& bot, Unit* candidate) override
    {
        return bot.OverpullGuard(candidate);
    }

    // In-combat maintenance (§2.4): Solo has no group opinion, so it ALWAYS DEFERS — returning
    // nullptr hands the tick to the spine's legacy in-combat handling (kill-credit, the
    // VISIBILITY_DISTANCE_SMALL range drop, the solo reselect), which runs byte-for-byte as it
    // does today. Solo never switches targets on its own; the B2 convergence that DOES switch
    // is TeamAuto's and never ran for a solo bot. (Contract: non-null = "commit to THIS unit";
    // nullptr = "defer to legacy" — see AiBotDoctrine.h.)
    Unit* MaintainTarget(AiBotAI& /*bot*/, Unit* /*victim*/) override
    {
        return nullptr;
    }

    // Shared-machinery opt-ins — Solo runs all three, matching today:
    bool UseStalemateBreaker() const override { return true; }  // HandleCombatStalemate
    bool UseOverpullRetreat()  const override { return true; }  // HandleOverpullRetreat
    bool UseTapRespect()       const override { return true; }  // drop foreign-tapped mobs

    char const* Name() const override { return "Solo"; }
};

} // anonymous namespace

// Factory hook for MakeDoctrine() (defined in AiBotDoctrine.cpp, the activation commit). The
// concrete class is kept file-local (anonymous namespace); only this one external-linkage
// entry point escapes the TU. Its declaration is added to AiBotDoctrine.h alongside
// MakeTeamDoctrine()/MakeDirectedDoctrine() when the central resolver+factory lands — defined
// here with external linkage now so the Solo TU is self-contained and additive.
std::unique_ptr<IEngagementDoctrine> MakeSoloDoctrine()
{
    return std::unique_ptr<IEngagementDoctrine>(new AiBotDoctrineSolo());
}
