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
    // Frozen logic (§2.2):
    //   posture == Companion || Puppet          -> Directed
    //   m_combatDirective.IsActive() && grouped -> TeamAuto
    //   else                                    -> Solo
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
        case DoctrineKind::Solo:     return MakeSoloDoctrine();
        case DoctrineKind::TeamAuto: return MakeTeamDoctrine();
        case DoctrineKind::Directed: return MakeDirectedDoctrine();
    }
    return MakeSoloDoctrine();   // unreachable — total switch; keeps the compiler happy
}
