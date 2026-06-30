/*
 * AiBotAITeamPlay.cpp — stateless group-combat resolvers (see AiBotAITeamPlay.h).
 *
 * v1: ResolveCombatTarget implements assist focus-fire. The god-bot coordinator (C#) nominates
 * an anchor member + stamps "assist mode" on every follower's CombatDirective; a follower reads
 * the anchor's live GetVictim() directly (every bot is a Player in the same mangosd process) and
 * focuses it. Literal shared GUID-lock, zero mob-GUID traffic on the wire. The anchor itself
 * (anchor_guid == self) gets no opinion here → selects normally → the team assists IT.
 */

#include "AiBotAITeamPlay.h"
#include "AiBotAI.h"
#include "Player.h"
#include "Group.h"
#include "Log.h"

// NOTE: VISIBILITY_DISTANCE_NORMAL is used by AiBotAI::SelectAttackTarget already; it resolves
// here through Player.h → Object.h on this fork. If a build flags it as undeclared, add the
// header that defines it (Object.h / GridDefines.h on your tree).

Unit* TeamPlay::ResolveCombatTarget(AiBotAI const& bot)
{
    CombatDirective const& dir = bot.m_combatDirective;
    if (!dir.IsActive())
        return nullptr;   // no opinion → caller runs its existing selection byte-for-byte

    Player* me = bot.GetBotPlayer();
    if (!me || !me->IsInWorld())
        return nullptr;

    // ── v1: assist ──
    if (dir.mode == COMBAT_MODE_ASSIST)
    {
        // I AM the anchor → no one to assist; select normally so the team assists me.
        if (dir.anchorGuidLow == me->GetGUIDLow())
            return nullptr;

        Group* pGroup = me->GetGroup();
        if (!pGroup)
            return nullptr;   // stamped but not actually grouped this tick → solo selection

        // Find the anchor member by low GUID (same Group-walk idiom as SelectAttackTarget step 3).
        Player* anchor = nullptr;
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* pMember = itr->getSource();
            if (pMember && pMember->GetGUIDLow() == dir.anchorGuidLow)
            {
                anchor = pMember;
                break;
            }
        }
        if (!anchor)
            return nullptr;   // anchor offline / not in our group this tick → solo selection

        Unit* pVictim = anchor->GetVictim();
        if (!pVictim)
            return nullptr;   // anchor between kills → solo-pick for a tick; re-assist next tick

        if (!bot.IsValidAssistTarget(pVictim))
            return nullptr;   // anchor's victim dead/invalid/non-hostile → don't focus it

        if (!me->IsWithinDist(pVictim, VISIBILITY_DISTANCE_NORMAL))
            return nullptr;   // anchor's victim out of our range → don't chase across the zone

        return pVictim;
    }

    return nullptr;
}

bool TeamPlay::ResolveCombatMove(AiBotAI const& bot, float& outX, float& outY, float& outZ)
{
    (void)bot; (void)outX; (void)outY; (void)outZ;
    return false;   // v1 STUB: no opinion — the bot positions exactly as it does solo.
}