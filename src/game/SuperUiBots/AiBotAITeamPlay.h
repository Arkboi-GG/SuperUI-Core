/*
 * AiBotAITeamPlay.h — stateless group-combat resolvers for AiBotAI.
 *
 * The ownership boundary (SuperUiBots design §3.7): TeamPlay owns group-DIVERGENT decisions
 * ("who do I attack", later "where do I stand"); AiBotAI owns everything a bot intrinsically
 * IS plus all execution/state/wire. These functions READ an AiBotAI (const ref) and the live
 * Group it can already see; they never move the bot, never touch m_currentTask, never emit a
 * bridge event. v1 = assist focus-fire.
 *
 * v1.1 (2026-07-02) update to the above: ResolveCombatTarget now writes ONE thing through the
 * const ref — a `mutable` sticky-assist memo (last-assisted victim guid + a tick budget, see
 * AiBotAI::m_lastAssistedVictimGuid / m_assistStickyTicks) so it can bridge the anchor's
 * between-kill gaps instead of yielding to solo-pick every time GetVictim() is momentarily
 * null. This is cache the resolver owns and nothing else reads, not task/execution/wire state —
 * so "mutates nothing [the rest of the AI or wire can observe]" still holds, even though
 * "mutates nothing at all" no longer does literally. See AiBotAITeamPlay.cpp for the trace.
 *
 * Forward decls only — no heavy includes here. The .cpp pulls AiBotAI.h / Player.h / Group.h.
 */

#ifndef MANGOS_AIBOTAI_TEAMPLAY_H
#define MANGOS_AIBOTAI_TEAMPLAY_H

class AiBotAI;
class Unit;

namespace TeamPlay
{
    // "Who do I attack?" — the group focus-fire decision. Returns the anchor's live victim,
    // or (v1.1) the last victim we assisted while the anchor is momentarily between kills,
    // when an assist directive is live and resolvable; nullptr ("no opinion") otherwise, so
    // the caller's existing selection runs unchanged. v1: assist only.
    Unit* ResolveCombatTarget(AiBotAI const& bot);

    // "Where do I move?" — the documented future seam (role positioning / move-to-ally).
    // v1 STUB: always returns false (no opinion); the bot positions exactly as it does solo.
    // Declared now so the ownership boundary is right from the first commit, and the movement
    // weave hook lands later with no re-plumbing.
    bool ResolveCombatMove(AiBotAI const& bot, float& outX, float& outY, float& outZ);
}

#endif