/*
 * AiBotAITeamPlay.h — group-combat resolvers for AiBotAI.
 *
 * v3 (doctrine split): the assist focus-fire decision (ResolveCombatTarget) + the sticky-assist
 * memo were RETIRED from here and absorbed into the TeamAuto engagement doctrine
 * (AiBotDoctrineTeam.cpp) — the single owner of every group-fight decision. What survives is the
 * documented future move seam, ResolveCombatMove ("where do I stand" — role positioning /
 * move-to-ally), declared now so the movement weave lands later with no re-plumbing.
 *
 * Forward decls only — no heavy includes here. The .cpp pulls AiBotAI.h.
 */

#ifndef MANGOS_AIBOTAI_TEAMPLAY_H
#define MANGOS_AIBOTAI_TEAMPLAY_H

class AiBotAI;

namespace TeamPlay
{
    // "Where do I move?" — the documented future seam (role positioning / move-to-ally).
    // v1 STUB: always returns false (no opinion); the bot positions exactly as it does solo.
    // Declared now so the ownership boundary is right and the movement weave hook lands later
    // with no re-plumbing.
    bool ResolveCombatMove(AiBotAI const& bot, float& outX, float& outY, float& outZ);
}

#endif
