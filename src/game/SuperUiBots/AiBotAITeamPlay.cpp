/*
 * AiBotAITeamPlay.cpp — stateless group-combat resolvers (see AiBotAITeamPlay.h).
 *
 * v1: ResolveCombatTarget implements assist focus-fire. The god-bot coordinator (C#) nominates
 * an anchor member + stamps "assist mode" on every follower's CombatDirective; a follower reads
 * the anchor's live GetVictim() directly (every bot is a Player in the same mangosd process) and
 * focuses it. Literal shared GUID-lock, zero mob-GUID traffic on the wire. The anchor itself
 * (anchor_guid == self) gets no opinion here → selects normally → the team assists IT.
 *
 * v1.1 (2026-07-02): closes the "anchor between kills" divergence gap (GAP B in the grouping
 * fix plan). Root cause was that a null anchor->GetVictim() — routine for a tick or two between
 * kills — immediately yielded to solo-pick, and once a follower started attacking its own pick
 * it stayed there. Fix is validity-gated sticky memory: remember the last mob we assisted, keep
 * returning it through IsValidAssistTarget's SAME dead/invalid/non-hostile/range gate the live
 * victim already goes through (so a mob the anchor just killed drops out with zero extra logic),
 * bounded by a small tick budget so a genuinely-disengaged anchor can't glue us to a stale target
 * forever. Deliberately NOT a wall-clock window — see AiBotAIMain.h's AIBOT_ASSIST_STICKY_MAX_TICKS
 * for why. Deliberately NOT a HostileRefManager threat-list fallback either (yet) — that's a
 * real future seam (documented below, at the point it would slot in) but adding it speculatively
 * now would be exactly the kind of dead-seam debt the C# cleanup already removed once.
 */

#include "AiBotAITeamPlay.h"
#include "AiBotAI.h"
#include "Player.h"
#include "Group.h"
#include "Creature.h"   // Map::GetCreature returns Creature* — need the complete type to upcast to Unit*
#include "Map.h"        // me->GetMap()->GetCreature(guid) — same idiom AiBotAICombat.cpp already uses for m_stalemateVictim
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
        {
            bot.m_lastAssistedVictimGuid.Clear();
            bot.m_assistStickyTicks = 0;
            return nullptr;   // stamped but not actually grouped this tick → solo selection
        }

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
        {
            bot.m_lastAssistedVictimGuid.Clear();
            bot.m_assistStickyTicks = 0;
            return nullptr;   // anchor offline / not in our group this tick → solo selection
        }

        Unit* pVictim = anchor->GetVictim();

        // ── v1.1: sticky across the anchor's between-kill gaps ──
        // A genuine live anchor victim is always authoritative: adopt it, remember it, and
        // reset the sticky safety-valve — whatever stickiness was doing before, the anchor
        // having a real target again takes priority over it.
        if (pVictim && bot.IsValidAssistTarget(pVictim) && me->IsWithinDist(pVictim, VISIBILITY_DISTANCE_NORMAL))
        {
            bot.m_lastAssistedVictimGuid = pVictim->GetObjectGuid();
            bot.m_assistStickyTicks = 0;
            return pVictim;
        }

        // No usable live anchor victim this tick. Hold the last mob we assisted instead of
        // yielding straight to solo-pick — this is what actually closes the "anchor between
        // kills" gap. Two gates, both must hold:
        //   - IsValidAssistTarget: self-clears the instant the remembered mob is dead/invalid/
        //     non-hostile, so a mob the anchor just killed drops out with no extra bookkeeping.
        //   - AIBOT_ASSIST_STICKY_MAX_TICKS: bounds how long we'll hold on if the anchor has
        //     simply stopped re-engaging (quest done, moved on) rather than being mid-kill —
        //     without this a follower could stay glued to an alive-but-abandoned mob forever.
        if (!bot.m_lastAssistedVictimGuid.IsEmpty() && bot.m_assistStickyTicks < AIBOT_ASSIST_STICKY_MAX_TICKS)
        {
            if (Creature* pSticky = me->GetMap()->GetCreature(bot.m_lastAssistedVictimGuid))
            {
                if (bot.IsValidAssistTarget(pSticky) && me->IsWithinDist(pSticky, VISIBILITY_DISTANCE_NORMAL))
                {
                    ++bot.m_assistStickyTicks;
                    return pSticky;
                }
            }
        }

        // Sticky memory exhausted or invalid → clear it and yield to solo selection.
        //
        // FUTURE SEAM (option 2 from the fix plan, not built): instead of yielding here, could
        // walk anchor->GetHostileRefManager() — same idiom AiBotAICombat.cpp's own
        // SelectAttackTarget already uses on itself (getFirst()/getSourceUnit()/next()) — for
        // the anchor's own highest/nearest threat and assist THAT. That keeps the team on the
        // anchor's fight even as the anchor itself retargets, not just bridging a brief gap.
        // Deferred until live testing shows sticky-alone isn't enough for the observed symptom.
        bot.m_lastAssistedVictimGuid.Clear();
        bot.m_assistStickyTicks = 0;
        return nullptr;
    }

    return nullptr;
}

bool TeamPlay::ResolveCombatMove(AiBotAI const& bot, float& outX, float& outY, float& outZ)
{
    (void)bot; (void)outX; (void)outY; (void)outZ;
    return false;   // v1 STUB: no opinion — the bot positions exactly as it does solo.
}