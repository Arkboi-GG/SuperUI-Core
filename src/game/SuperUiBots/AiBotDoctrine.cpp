/*
 * AiBotDoctrine.cpp — the doctrine resolver + factory (Layer D, §2.2).
 *
 * ResolveDoctrine is the single "top-top level check": one place, evaluated each behaviour tick
 * by the spine (RefreshDoctrine in UpdateAI), that decides which of the three engagement
 * doctrines a bot is under. MakeDoctrine constructs the chosen one via the per-TU factory hooks;
 * the concrete classes stay file-local in their own .cpp so nothing else can reach them.
 *
 * The bot owns std::unique_ptr<IEngagementDoctrine> m_doctrine and swaps it only when the kind
 * changes — so doctrine-transient state (TeamAuto's sticky memo + pull-hold counter) resets by
 * construction on every mode switch.
 *
 * Line endings: LF (C++ repo convention).
 */

#include "AiBotDoctrine.h"
#include "AiBotAIMain.h"
#include "Player.h"
#include "Group.h"

DoctrineKind ResolveDoctrine(AiBotAI const& bot)
{
    // Frozen logic (§2.2), extended 2026-07-07 with the one live-human override on top:
    //   FindPartyBoss() resolves (a REAL player is in the group) -> PlayerParty
    //   posture == Companion || Puppet          -> Directed
    //   m_combatDirective.IsActive() && grouped -> TeamAuto
    //   else                                    -> Solo
    //
    // [PLAYERPARTY] ranks ABOVE everything: a human /invite is the entire control plane for
    // escort mode, and a live human must outrank both a stale C# assist directive (the
    // coordinator also stands down off the pparty STATE echo, plus the bridge Form/Disband
    // guards refuse to touch a player-led party -- this is the third belt) and, for now, the
    // present-but-dark M2 Directed branch (revisit the ranking when the conduct substrate
    // actually lands). Leaving the party un-resolves FindPartyBoss and this drops straight
    // back to the frozen ladder next behaviour tick -- swap == reset, as always.
    if (bot.FindPartyBoss())
        return DoctrineKind::PlayerParty;

    // [SUI] The owner's non-autonomy decree, enforced LOCALLY (2026-08-10). An unattended
    // real character — the body its human left behind to possess a bot or fly the free view —
    // must never pick its own goals. Nothing was actually holding it: pparty=1 is only ever an
    // echo TO the brain, and the theft wall keeps this AI off the bridge entirely, so the echo
    // has no reader. With nobody possessed, FindPartyBoss finds no OTHER real member in a
    // group of bots, so this fell through the ladder to Solo and the body went grinding across
    // the zone — logged as "[AIBOT-DOCTRINE] Tesfff: (none) -> Solo" the moment its human
    // entered the free view, with the rest of the party dutifully escorting the runaway.
    //
    // PlayerParty is precisely the hold wanted, with no new behaviour invented: formation on
    // whoever is being driven when someone is, DoPartyFollow's early return (no boss = stand
    // still) when nobody is, combat assist either way, and — because the branch stands the
    // whole task machinery down — no grind, no patrol, no wander. CMSG_SUI_ORDER injections
    // remain the only thing that moves it deliberately, exactly as the decree says.
    if (bot.IsUnattendedRealCharacter())
        return DoctrineKind::PlayerParty;
    //
    // Posture source is the M2 ConductState. Until it lands, every bot is AUTONOMOUS (§3), so the
    // Directed branch below is present-but-dark and the resolver only ever picks TeamAuto or Solo
    // — which is exactly today's behaviour split. Wiring M2 = replacing this one line with
    // bot.GetConductPosture().
    ConductPosture const posture = ConductPosture::Autonomous;   // TODO(M2): bot.GetConductPosture();

    if (posture == ConductPosture::Companion || posture == ConductPosture::Puppet)
        return DoctrineKind::Directed;

    Player* me = bot.GetBotPlayer();   // GetBotPlayer() is const but returns the non-const `me`
    bool const grouped = (me != nullptr && me->GetGroup() != nullptr);

    if (bot.m_combatDirective.IsActive() && grouped)
        return DoctrineKind::TeamAuto;

    return DoctrineKind::Solo;
}

std::unique_ptr<IEngagementDoctrine> MakeDoctrine(DoctrineKind kind)
{
    switch (kind)
    {
        case DoctrineKind::Solo:        return MakeSoloDoctrine();
        case DoctrineKind::TeamAuto:    return MakeTeamDoctrine();
        case DoctrineKind::Directed:    return MakeDirectedDoctrine();
        case DoctrineKind::PlayerParty: return MakePlayerPartyDoctrine();
    }
    return MakeSoloDoctrine();   // unreachable — total switch; keeps the compiler happy
}