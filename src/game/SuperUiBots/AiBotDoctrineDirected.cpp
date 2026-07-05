/*
 * AiBotDoctrineDirected.cpp — the Directed engagement doctrine (Layer D, §2.1).
 *
 * State: companion / puppet posture — a real player (or the RTS user) is the authority. This
 * is the v1 skeleton per §2.3: orders and the player command targeting; there is NO autonomous
 * pull path to leak through, and no self-preservation teleports/flees a player didn't ask for.
 *
 * ACTIVATION: the doctrine resolver only returns Directed when conduct posture is COMPANION or
 * PUPPET (§2.2). That posture arrives with the M2 conduct substrate (SET_CONDUCT + ConductState);
 * until then every bot is AUTONOMOUS, so this doctrine is present-but-dark — constructible via
 * MakeDoctrine(DoctrineKind::Directed), but never selected yet. It ships now so the three-state
 * split is complete and the factory is total (no Directed-shaped hole), not because M1 routes to
 * it.
 *
 * How orders reach the bot (why AcquireTarget/MaintainTarget stay quiet): direct orders are
 * bridge commands the spine already executes — ATTACK_TARGET (BridgeHandleAttackTarget pins
 * me->GetVictim()), MOVE_TO, INTERACT_NPC, USE_GAMEOBJECT. PUPPET adds not new commands but the
 * REMOVAL of autonomy that would fight them: this doctrine issues no acquisitions and forces no
 * retargets, so an order-pinned victim simply persists via the spine's normal victim handling.
 *
 * v1 vs later (§7.5, open items 3/6): COMPANION's "assist the player's target" uses the same
 * group-state read the god-bot uses, with the REAL PLAYER as anchor — but real-player-group
 * visibility (the bot must see me->GetGroup() truth for a player-formed group) is an M7 item, so
 * v1 keeps MaintainTarget deferring (hold the ordered target) rather than half-wiring an assist
 * with no anchor. When M7 lands, MaintainTarget grows the player-anchor assist; the interface
 * does not change.
 *
 * Line endings: LF (C++ repo convention).
 */

#include "AiBotDoctrine.h"
#include "AiBotAIMain.h"

#include <memory>

namespace
{

class AiBotDoctrineDirected final : public IEngagementDoctrine
{
public:
    // No autonomous pull path exists (§2.3). The bot pulls only what an order names, and orders
    // arrive as bridge commands the spine executes directly — never through here.
    Unit* AcquireTarget(AiBotAI& /*bot*/) override
    {
        return nullptr;
    }

    // Unordered pulls do not exist to veto; ordered pulls are never vetoed. Never returns a hold.
    bool HoldPull(AiBotAI& /*bot*/, Unit* /*candidate*/) override
    {
        return false;
    }

    // Defer: the spine keeps swinging at whatever an order pinned (me->GetVictim()). v1 does no
    // autonomous retarget; the COMPANION "assist the player's target" via the real-player anchor
    // is M7 (see the file header).
    Unit* MaintainTarget(AiBotAI& /*bot*/, Unit* /*victim*/) override
    {
        return nullptr;
    }

    // Under player control, the bot performs no self-preservation the player didn't order:
    // no stalemate teleports, no overpull flees. Tap-respect is a param later (PUPPET: off).
    bool UseStalemateBreaker() const override { return false; }
    bool UseOverpullRetreat()  const override { return false; }
    bool UseTapRespect()       const override { return false; }

    char const* Name() const override { return "Directed"; }
};

} // anonymous namespace

// Factory hook consumed by MakeDoctrine() in AiBotDoctrine.cpp.
std::unique_ptr<IEngagementDoctrine> MakeDirectedDoctrine()
{
    return std::unique_ptr<IEngagementDoctrine>(new AiBotDoctrineDirected());
}
