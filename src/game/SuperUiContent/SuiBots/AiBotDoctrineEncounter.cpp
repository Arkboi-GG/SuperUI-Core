/*
 * AiBotDoctrineEncounter.cpp — the EncounterPlay engagement doctrine (Layer D,
 * PLAN_19 M-D).
 *
 * State: a raid plan is loaded (M-C) and a combat directive is active — the fleet
 * is raiding under doctrine. EncounterPlay COMPOSES TeamAuto rather than replacing
 * it: every group-fight decision the god-bot's doctrine makes (resolver-first
 * acquisition, B3 pull-hold, sticky assist) is delegated to an inner TeamAuto
 * instance, and exactly ONE decision is layered on top — MaintainTarget prefers
 * this bot's assigned ADD when its plan puts it on add duty and a live add is in
 * the fight. Composition keeps the group behaviour bit-compatible for every bot
 * whose plan carries no add assignment.
 *
 * Selected by ResolveDoctrine when the TeamAuto conditions hold AND the bot has an
 * adopted raid plan; ranked below PlayerParty (a live human always outranks the
 * plan). Doctrine-transient state resets by construction on every swap, inner
 * TeamAuto included.
 *
 * Line endings: LF (C++ repo convention).
 */

#include "AiBotDoctrine.h"
#include "AiBotAIMain.h"
#include "AiBotCircuit.h" // [CIRCUIT] probe macros (CIRCUIT_BOARD.md)
#include "Player.h"       // [CIRCUIT] GetGUIDLow on the bot player (probe guid)

#include <memory>

namespace
{

// [CIRCUIT] Null-safe guid for probes; evaluated only inside an armed probe.
uint32 CbGuid(AiBotAI const& bot)
{
    Player* p = bot.GetBotPlayer();
    return p ? p->GetGUIDLow() : 0;
}

class AiBotDoctrineEncounterPlay final : public IEngagementDoctrine
{
public:
    Unit* AcquireTarget(AiBotAI& bot) override
    {
        return m_inner->AcquireTarget(bot);
    }

    bool HoldPull(AiBotAI& bot, Unit* candidate) override
    {
        return m_inner->HoldPull(bot, candidate);
    }

    // The one layered decision: an add-duty bot commits to its assigned add while
    // one lives; otherwise the inner TeamAuto keeps the group opinion.
    Unit* MaintainTarget(AiBotAI& bot, Unit* victim) override
    {
        if (Unit* add = bot.RaidPlanPreferredAdd())
        {
            CB_HIT(CbGuid(bot), "cpp-doctrine: encounter, committing to assigned add");
            return add;
        }
        return m_inner->MaintainTarget(bot, victim);
    }

    bool UseStalemateBreaker() const override { return m_inner->UseStalemateBreaker(); }
    bool UseOverpullRetreat()  const override { return m_inner->UseOverpullRetreat(); }
    bool UseTapRespect()       const override { return m_inner->UseTapRespect(); }
    bool HoldingForTeam()      const override { return m_inner->HoldingForTeam(); }

    char const* Name() const override { return "EncounterPlay"; }

private:
    std::unique_ptr<IEngagementDoctrine> m_inner = MakeTeamDoctrine();
};

} // namespace

std::unique_ptr<IEngagementDoctrine> MakeEncounterDoctrine()
{
    return std::make_unique<AiBotDoctrineEncounterPlay>();
}
