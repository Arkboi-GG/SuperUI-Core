/* SuperUI RTS faction Honor (R2). */

#include "SuiHonor.h"

#include <atomic>
#include <algorithm>

#include "AiBotAIMain.h"
#include "Database/DBCEnums.h"
#include "Database/DBCStructure.h"
#include "Objects/Creature.h"
#include "Objects/Player.h"
#include "Objects/Unit.h"
#include "SuiPossess.h"
#include "SuiRts.h"

namespace SuiHonor
{

namespace
{
    std::atomic<int64> s_playerWeight{10};
    std::atomic<int64> s_botWeight{5};
    std::atomic<int64> s_npcWeight{1};
    std::atomic<int64> s_eliteWeight{3};
    std::atomic<bool> s_suppressBotHk{true};

    uint8 TeamIndex(Player const* player)
    {
        return player && player->GetTeam() == HORDE ? 1 : 0;
    }

    int64 NonNegativeRule(char const* key, int64 defaultValue)
    {
        return std::max<int64>(0, SuiRts::GetKVInt(key, defaultValue));
    }

    bool IsAiBot(Player const* player)
    {
        return player && player->GetSession() && player->GetSession()->GetBot() &&
            dynamic_cast<AiBotAI*>(const_cast<Player*>(player)->AI()) != nullptr;
    }
}

void LoadRuleset()
{
    s_playerWeight.store(NonNegativeRule("honor.weight.player", 10), std::memory_order_relaxed);
    s_botWeight.store(NonNegativeRule("honor.weight.bot", 5), std::memory_order_relaxed);
    s_npcWeight.store(NonNegativeRule("honor.weight.npc", 1), std::memory_order_relaxed);
    s_eliteWeight.store(NonNegativeRule("honor.weight.npc_elite", 3), std::memory_order_relaxed);
    s_suppressBotHk.store(SuiRts::GetKVInt("honor.suppress_bot_hk", 1) != 0,
        std::memory_order_relaxed);
}

void OnUnitKill(Unit* killer, Unit* victim)
{
    // Deliberately keep the cold path to two cheap gates. The weight cache is
    // atomic because this hook runs on parallel map threads.
    if (!SuiPossess::RtsWorldState() || !SuiRts::HonorEnabled())
        return;
    if (!killer || !victim || killer == victim)
        return;

    // Credit the player responsible for the killing blow, including a pet's
    // owner. Do not use Unit::Kill's later loot recipient: a tag is not a kill.
    Player* killerPlayer = killer->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!killerPlayer || killerPlayer == victim)
        return;
    if (!killerPlayer->IsHostileTo(victim))
        return;

    uint8 const team = TeamIndex(killerPlayer);
    int64 weight = 0;

    if (Player* playerVictim = victim->ToPlayer())
    {
        if (playerVictim->GetTeam() == killerPlayer->GetTeam())
            return; // duels, same-faction damage, and suicides never fund a side
        weight = IsAiBot(playerVictim)
            ? s_botWeight.load(std::memory_order_relaxed)
            : s_playerWeight.load(std::memory_order_relaxed);
    }
    else if (Creature* creatureVictim = victim->ToCreature())
    {
        // "Faction NPC" is exact: the template must belong exclusively to the
        // opposing player faction. Ordinary hostile wildlife, neutral mobs,
        // pets, guardians, and summons do not mint RTS Honor.
        if (creatureVictim->IsPet() || creatureVictim->IsTotem() ||
            creatureVictim->IsTemporarySummon() ||
            creatureVictim->GetCharmerOrOwnerPlayerOrPlayerItself())
            return;
        FactionTemplateEntry const* faction = creatureVictim->GetFactionTemplateEntry();
        if (!faction)
            return;
        uint32 const sideMask = faction->ourMask & (FACTION_MASK_ALLIANCE | FACTION_MASK_HORDE);
        uint32 const enemyMask = team == 0 ? FACTION_MASK_HORDE : FACTION_MASK_ALLIANCE;
        if (sideMask != enemyMask)
            return;
        weight = creatureVictim->IsElite()
            ? s_eliteWeight.load(std::memory_order_relaxed)
            : s_npcWeight.load(std::memory_order_relaxed);
    }

    if (weight > 0)
        SuiRts::AddHonor(team, weight);
}

bool SuppressVanillaHonor(Player const* recipient, Player const* victim)
{
    return SuiPossess::RtsWorldState() && SuiRts::HonorEnabled() &&
        s_suppressBotHk.load(std::memory_order_relaxed) &&
        IsAiBot(recipient) && IsAiBot(victim);
}

} // namespace SuiHonor
