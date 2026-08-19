/* SuperUI RTS AiBot heroes (R2). */

#include "SuiHero.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AiBotAIMain.h"
#include "Database/DatabaseEnv.h"
#include "Database/DBCStructure.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Objects/Player.h"
#include "Objects/Unit.h"
#include "Server/WorldSession.h"
#include "Spells/SpellAuraDefines.h"
#include "Spells/SpellDefines.h"
#include "Spells/SpellEntry.h"
#include "Spells/SpellMgr.h"
#include "SuiWorldState.h"
#include "SuiRts.h"

namespace SuiHero
{
namespace
{
    static uint32 const FIRST_HERO_SPELL = 51001;
    static uint32 const LAST_HERO_SPELL = 51005;

    struct Rule
    {
        uint32 DeclareCost = 0;
        uint32 ReviveFee = 0;
        uint32 SpellId = 0;
        uint16 ScalePercent = 100;
        uint16 DamagePercent = 100;
        bool Valid = false;
    };

    struct Hero
    {
        uint8 Team = 0;
        uint8 Level = 1;
        bool Dead = false;
    };

    std::array<Rule, 6> s_rules;
    std::unordered_map<uint32, Hero> s_heroes;
    std::unordered_set<uint32> s_pendingDeaths;
    std::unordered_set<uint32> s_reviving;
    std::mutex s_mutex;
    uint16 s_slotCap = 4;

    bool Active()
    {
        return SuiWorldState::RtsWorldState() && SuiRts::HeroesEnabled();
    }

    uint8 TeamIndex(Player const* player)
    {
        return player && player->GetTeam() == HORDE ? 1 : 0;
    }

    bool IsBotSession(Player const* player)
    {
        return player && player->GetSession() && player->GetSession()->GetBot();
    }

    bool IsAiBot(Player const* player)
    {
        return IsBotSession(player) &&
            dynamic_cast<AiBotAI*>(const_cast<Player*>(player)->AI()) != nullptr;
    }

    bool IsLiveLocked(uint32 guidLow, Hero const& hero)
    {
        return !hero.Dead && s_pendingDeaths.find(guidLow) == s_pendingDeaths.end() &&
            s_reviving.find(guidLow) == s_reviving.end();
    }

    bool ValidateSpell(uint8 level, Rule const& rule)
    {
        if (rule.SpellId != FIRST_HERO_SPELL + level - 1 ||
            rule.ScalePercent < 100 || rule.ScalePercent > 200 ||
            rule.DamagePercent < 100 || rule.DamagePercent > 200)
            return false;

        SpellEntry const* spell = sSpellMgr.GetSpellEntry(rule.SpellId);
        uint32 const requiredAttributes = uint32(SPELL_ATTR_PASSIVE) |
            uint32(SPELL_ATTR_NO_AURA_CANCEL);
        if (!spell || spell->School != 0 || spell->Attributes != requiredAttributes ||
            spell->AttributesEx != 0 || spell->AttributesEx2 != 0 ||
            spell->AttributesEx3 != 0 || spell->AttributesEx4 != 0 ||
            spell->Targets != 0 || spell->CastingTimeIndex != 1 ||
            spell->DurationIndex != 21 || spell->GetDuration() != -1 ||
            spell->rangeIndex != 1 || spell->StackAmount != 1 ||
            spell->procFlags != 0 || spell->procChance != 0 ||
            spell->procCharges != 0 || spell->Custom != 0 ||
            spell->EquippedItemClass != -1 ||
            spell->EquippedItemSubClassMask != 0 ||
            spell->EquippedItemInventoryTypeMask != 0)
            return false;

        // MangosSuperUI authors these effects in a fixed order. Validate the
        // exact persisted shape so Core cannot enable a spell that Web would
        // reject or that carries an extra target, periodic, or trigger effect.
        return spell->Effect[0] == SPELL_EFFECT_APPLY_AURA &&
            spell->Effect[1] == SPELL_EFFECT_APPLY_AURA &&
            spell->Effect[2] == 0 &&
            spell->EffectBaseDice[0] == 1 &&
            spell->EffectBaseDice[1] == 1 &&
            spell->EffectDieSides[0] == 1 &&
            spell->EffectDieSides[1] == 1 &&
            spell->EffectBasePoints[0] == int32(rule.ScalePercent) - 101 &&
            spell->EffectBasePoints[1] == int32(rule.DamagePercent) - 101 &&
            spell->EffectImplicitTargetA[0] == TARGET_UNIT_CASTER &&
            spell->EffectImplicitTargetA[1] == TARGET_UNIT_CASTER &&
            spell->EffectImplicitTargetB[0] == 0 &&
            spell->EffectImplicitTargetB[1] == 0 &&
            spell->EffectApplyAuraName[0] == SPELL_AURA_MOD_SCALE &&
            spell->EffectApplyAuraName[1] == SPELL_AURA_MOD_DAMAGE_PERCENT_DONE &&
            spell->EffectMiscValue[0] == 0 &&
            uint32(spell->EffectMiscValue[1]) == uint32(SPELL_SCHOOL_MASK_ALL) &&
            spell->EffectAmplitude[0] == 0 &&
            spell->EffectAmplitude[1] == 0 &&
            spell->EffectTriggerSpell[0] == 0 &&
            spell->EffectTriggerSpell[1] == 0;
    }

    void ApplyConfiguredAura(Player* player, uint8 desiredLevel)
    {
        if (!player || !IsAiBot(player))
            return;

        uint32 desiredSpell = desiredLevel > 0 && desiredLevel < s_rules.size()
            ? s_rules[desiredLevel].SpellId : 0;
        for (uint32 spellId = FIRST_HERO_SPELL; spellId <= LAST_HERO_SPELL; ++spellId)
            if (spellId != desiredSpell && player->HasAura(spellId))
                player->RemoveAurasDueToSpell(spellId);

        if (desiredSpell && !player->HasAura(desiredSpell))
            if (!player->AddAura(desiredSpell,
                ADD_AURA_PASSIVE | ADD_AURA_PERMANENT | ADD_AURA_POSITIVE, player))
                sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                    "[SUI-RTS] failed to apply hero aura %u to %s",
                    desiredSpell, player->GetName());
    }

    void RefreshAura(uint32 guidLow)
    {
        Player* player = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guidLow));
        if (!player)
            return;

        uint8 desiredLevel = 0;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto itr = s_heroes.find(guidLow);
            if (itr != s_heroes.end() && IsLiveLocked(guidLow, itr->second))
                desiredLevel = itr->second.Level;
        }
        ApplyConfiguredAura(player, desiredLevel);
    }

    void DrainDeaths()
    {
        std::vector<uint32> changed;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            changed.reserve(s_pendingDeaths.size());
            for (auto pending = s_pendingDeaths.begin(); pending != s_pendingDeaths.end();)
            {
                uint32 guidLow = *pending;
                if (s_reviving.find(guidLow) != s_reviving.end())
                {
                    ++pending;
                    continue;
                }
                auto itr = s_heroes.find(guidLow);
                if (itr != s_heroes.end() && !itr->second.Dead)
                {
                    itr->second.Dead = true;
                    changed.push_back(guidLow);
                }
                pending = s_pendingDeaths.erase(pending);
            }
        }

        for (uint32 guidLow : changed)
        {
            CharacterDatabase.DirectPExecute(
                "UPDATE `superui_heroes` SET `dead`=1 WHERE `guid`=%u", guidLow);
            RefreshAura(guidLow);
        }
    }
}

bool LoadRuleset()
{
    std::array<Rule, 6> rules;
    std::array<bool, 6> seenRules{};
    std::unordered_map<uint32, Hero> heroes;
    uint32 ruleRows = 0;
    uint32 validRules = 0;

    if (auto result = CharacterDatabase.Query(
        "SELECT `hero_level`,`declare_cost`,`revive_fee`,`spell_id`,"
        "`scale_percent`,`damage_percent` FROM `superui_rules_hero`"))
    {
        do
        {
            Field* fields = result->Fetch();
            ++ruleRows;
            uint32 rawLevel = fields[0].GetUInt32();
            if (rawLevel == 0 || rawLevel >= rules.size() || seenRules[rawLevel])
                continue;
            uint8 level = uint8(rawLevel);
            seenRules[level] = true;
            Rule& rule = rules[level];
            rule.DeclareCost = fields[1].GetUInt32();
            rule.ReviveFee = fields[2].GetUInt32();
            rule.SpellId = fields[3].GetUInt32();
            rule.ScalePercent = fields[4].GetUInt16();
            rule.DamagePercent = fields[5].GetUInt16();
            rule.Valid = ValidateSpell(level, rule);
            if (rule.Valid)
                ++validRules;
            else
                sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                    "[SUI-RTS] invalid hero rule/spell at level %u (spell %u)",
                    level, rule.SpellId);
        } while (result->NextRow());
    }

    bool valid = ruleRows == 5 && validRules == 5;
    if (valid)
        if (auto result = CharacterDatabase.Query(
            "SELECT `guid`,`team`,`hero_level`,`dead` FROM `superui_heroes`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 guidLow = fields[0].GetUInt32();
                uint8 team = uint8(fields[1].GetUInt32());
                uint8 level = uint8(fields[2].GetUInt32());
                if (!guidLow || team > 1 || level == 0 || level >= rules.size() || !rules[level].Valid)
                    continue;
                Hero hero;
                hero.Team = team;
                hero.Level = level;
                hero.Dead = fields[3].GetUInt32() != 0;
                heroes[guidLow] = hero;
            } while (result->NextRow());
        }

    int64 configuredSlots = SuiRts::GetKVInt("hero.slots_fixed", 4);
    std::lock_guard<std::mutex> lock(s_mutex);
    s_rules = rules;
    s_heroes.swap(heroes);
    s_pendingDeaths.clear();
    s_reviving.clear();
    // SMSG_SUI_RTS_STATE has one u8 hero-count for both factions. Keeping the
    // per-side cap at 127 guarantees the complete two-faction roster fits.
    s_slotCap = uint16(std::max<int64>(1, std::min<int64>(127, configuredSlots)));
    return valid;
}

void Tick()
{
    if (Active())
        DrainDeaths();
}

void Shutdown()
{
    if (Active())
        DrainDeaths();
}

void OnUnitKill(Unit* victim)
{
    if (!Active() || !victim)
        return;
    Player* player = victim->ToPlayer();
    if (!IsAiBot(player))
        return;

    std::lock_guard<std::mutex> lock(s_mutex);
    uint32 guidLow = player->GetGUIDLow();
    auto itr = s_heroes.find(guidLow);
    if (itr != s_heroes.end() && !itr->second.Dead)
        s_pendingDeaths.insert(guidLow);
}

void OnPlayerWorldEnter(Player* player)
{
    if (!Active() || !IsAiBot(player))
        return;

    bool heldDead = false;
    bool newlyDead = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto itr = s_heroes.find(player->GetGUIDLow());
        if (itr != s_heroes.end())
        {
            if (!itr->second.Dead && player->IsDead())
            {
                itr->second.Dead = true;
                newlyDead = true;
            }
            heldDead = itr->second.Dead;
        }
    }
    // A crash can preserve the character death before the periodic hero-row
    // drain. Reconcile that direction too, synchronously, before bot AI gets a
    // chance to take its ordinary free resurrection path.
    if (newlyDead)
        CharacterDatabase.DirectPExecute(
            "UPDATE `superui_heroes` SET `dead`=1 WHERE `guid`=%u",
            player->GetGUIDLow());
    // An R1 boot deliberately ignores preserved R2 hero rows, so that bot may
    // have been revived there. On the next R2 boot the persisted dead flag is
    // authoritative: restore the normal dead path before paid revive is used.
    if (heldDead && player->IsAlive())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[SUI-RTS] reconciling alive bot %s to its persisted dead hero state",
            player->GetName());
        player->Kill(player, nullptr, false);
    }
    RefreshAura(player->GetGUIDLow());
}

bool BlocksResurrection(Player const* player)
{
    // Login corpse recovery runs before PlayerBotMgr attaches AiBotAI. Session
    // bot identity plus the persisted hero row is therefore the safe hold
    // boundary; declaration and all active hero operations remain AiBotAI-only.
    if (!Active() || !IsBotSession(player))
        return false;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto itr = s_heroes.find(player->GetGUIDLow());
    return itr != s_heroes.end() &&
        (player->IsDead() || itr->second.Dead ||
         s_pendingDeaths.find(player->GetGUIDLow()) != s_pendingDeaths.end() ||
         s_reviving.find(player->GetGUIDLow()) != s_reviving.end());
}

uint16 Fielded(uint8 teamIdx)
{
    if (!Active())
        return 0;
    std::lock_guard<std::mutex> lock(s_mutex);
    uint32 count = 0;
    for (auto const& row : s_heroes)
        if (row.second.Team == (teamIdx & 1))
            ++count;
    return uint16(std::min<uint32>(65535, count));
}

uint16 SlotCap(uint8 /*teamIdx*/)
{
    if (!Active())
        return 0;
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_slotCap;
}

void SnapshotRows(std::vector<Snapshot>& rows)
{
    rows.clear();
    if (!Active())
        return;
    std::lock_guard<std::mutex> lock(s_mutex);
    rows.reserve(s_heroes.size());
    for (auto const& row : s_heroes)
    {
        Snapshot item;
        item.GuidLow = row.first;
        item.Team = row.second.Team;
        item.Level = row.second.Level;
        item.Dead = row.second.Dead || s_pendingDeaths.find(row.first) != s_pendingDeaths.end() ||
            s_reviving.find(row.first) != s_reviving.end();
        rows.push_back(item);
    }
    std::sort(rows.begin(), rows.end(), [](Snapshot const& left, Snapshot const& right)
    {
        return left.GuidLow < right.GuidLow;
    });
}

uint8 HandleAction(Player* actor, uint8 action, uint64 subjectRaw, int64& poolAfter)
{
    if (!Active() || !actor || !actor->IsInWorld() || !actor->GetSession() ||
        actor->GetSession()->GetBot())
        return 4;

    ObjectGuid requested(subjectRaw);
    if (!requested.IsPlayer())
        return 3;
    Player* subject = sObjectMgr.GetPlayer(
        ObjectGuid(HIGHGUID_PLAYER, requested.GetCounter()));
    if (!subject || !subject->IsInWorld() || subject->GetTeam() != actor->GetTeam() ||
        !IsAiBot(subject))
        return 3;

    uint8 team = TeamIndex(actor);
    uint32 guidLow = subject->GetGUIDLow();
    uint32 cost = 0;
    uint8 targetLevel = 0;

    if (action == 3)
    {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto itr = s_heroes.find(guidLow);
            if (itr == s_heroes.end() || itr->second.Team != team ||
                (!itr->second.Dead && s_pendingDeaths.find(guidLow) == s_pendingDeaths.end()) ||
                s_reviving.find(guidLow) != s_reviving.end() || subject->IsAlive())
                return 3;
            Rule const& rule = s_rules[itr->second.Level];
            if (!rule.Valid)
                return 4;
            targetLevel = itr->second.Level;
            cost = rule.ReviveFee;
        }

        WorldSafeLocsEntry const* grave = sObjectMgr.GetClosestGraveYard(
            subject->GetPositionX(), subject->GetPositionY(), subject->GetPositionZ(),
            subject->GetMapId(), subject->GetTeam());
        if (!grave)
            return 3;
        if (!SuiRts::TrySpendHonor(team, cost, &poolAfter))
            return 1;

        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto itr = s_heroes.find(guidLow);
            if (itr == s_heroes.end() || itr->second.Team != team ||
                (!itr->second.Dead && s_pendingDeaths.find(guidLow) == s_pendingDeaths.end()) ||
                s_reviving.find(guidLow) != s_reviving.end())
            {
                SuiRts::RefundHonor(team, cost, &poolAfter);
                return 3;
            }
            s_reviving.insert(guidLow);
        }

        if (subject->GetDeathState() == CORPSE)
            subject->BuildPlayerRepop();
        float orientation = subject->GetOrientation();
        if (float facing = sObjectMgr.GetWorldSafeLocFacing(grave->ID))
            orientation = facing;
        if (!subject->TeleportTo(grave->map_id, grave->x, grave->y, grave->z, orientation,
            TELE_TO_NOT_UNSUMMON_PET))
        {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_reviving.erase(guidLow);
            }
            SuiRts::RefundHonor(team, cost, &poolAfter);
            return 3;
        }

        subject->ResurrectPlayer(1.0f, false);
        subject->CombatStop(true);
        subject->SpawnCorpseBones();
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto itr = s_heroes.find(guidLow);
            if (itr != s_heroes.end())
                itr->second.Dead = false;
            s_pendingDeaths.erase(guidLow);
            s_reviving.erase(guidLow);
        }
        CharacterDatabase.DirectPExecute(
            "UPDATE `superui_heroes` SET `dead`=0 WHERE `guid`=%u", guidLow);
        ApplyConfiguredAura(subject, targetLevel);
        return 0;
    }

    if (action != 1 && action != 2)
        return 4;
    if (!subject->IsAlive())
        return 3;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto itr = s_heroes.find(guidLow);
        if (action == 1)
        {
            if (itr != s_heroes.end())
                return 3;
            uint32 fielded = 0;
            for (auto const& row : s_heroes)
                if (row.second.Team == team)
                    ++fielded;
            if (fielded >= s_slotCap)
                return 2;
            targetLevel = 1;
        }
        else
        {
            if (itr == s_heroes.end() || itr->second.Team != team ||
                !IsLiveLocked(guidLow, itr->second) || itr->second.Level >= 5)
                return 3;
            targetLevel = itr->second.Level + 1;
        }
        Rule const& rule = s_rules[targetLevel];
        if (!rule.Valid)
            return 4;
        cost = rule.DeclareCost;
    }

    if (!SuiRts::TrySpendHonor(team, cost, &poolAfter))
        return 1;
    if (!subject->IsAlive())
    {
        SuiRts::RefundHonor(team, cost, &poolAfter);
        return 3;
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto itr = s_heroes.find(guidLow);
        bool valid = action == 1
            ? itr == s_heroes.end()
            : itr != s_heroes.end() && itr->second.Team == team &&
              IsLiveLocked(guidLow, itr->second) && itr->second.Level + 1 == targetLevel;
        if (!valid)
        {
            SuiRts::RefundHonor(team, cost, &poolAfter);
            return 3;
        }
        if (action == 1)
        {
            Hero hero;
            hero.Team = team;
            hero.Level = targetLevel;
            hero.Dead = false;
            s_heroes[guidLow] = hero;
        }
        else
            itr->second.Level = targetLevel;
    }

    if (action == 1)
        CharacterDatabase.DirectPExecute(
            "REPLACE INTO `superui_heroes` (`guid`,`team`,`hero_level`,`dead`,`declared_at`) "
            "VALUES (%u,%u,%u,0,UNIX_TIMESTAMP())", guidLow, team, targetLevel);
    else
        CharacterDatabase.DirectPExecute(
            "UPDATE `superui_heroes` SET `hero_level`=%u WHERE `guid`=%u",
            targetLevel, guidLow);
    ApplyConfiguredAura(subject, targetLevel);
    return 0;
}
}
