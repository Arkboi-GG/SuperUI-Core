/*
 * AiBotAITeamPlay.cpp — group-combat resolvers (see AiBotAITeamPlay.h).
 *
 * v3 (doctrine split): ResolveCombatTarget — the assist focus-fire read + the sticky-assist memo
 * — was RETIRED here and absorbed into the TeamAuto engagement doctrine (AiBotDoctrineTeam.cpp),
 * which is now the single owner of every group-fight decision (resolver-first acquisition, the B3
 * wait-for-anchor pull-hold, B2 mid-combat convergence, and the sticky bridge). The sticky memo +
 * pull-hold counter moved onto that doctrine instance (they were `mutable`/plain members on
 * AiBotAI); its four dispatch sites and the two SelectGrind/SelectAttack prefixes that used to
 * call this function are gone. What remains in this TU is the documented future move seam,
 * ResolveCombatMove, so the ownership boundary and the header stay stable for the positioning
 * weave that lands later.
 */

#include "AiBotAITeamPlay.h"
#include "AiBotAI.h"

bool TeamPlay::ResolveCombatMove(AiBotAI const& bot, float& outX, float& outY, float& outZ)
{
    (void)bot; (void)outX; (void)outY; (void)outZ;
    return false;   // v1 STUB: no opinion — the bot positions exactly as it does solo.
}
