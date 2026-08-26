#include "AiBotTalents.h"

#include "PlayerBotMgr.h"
#include "Player.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"

#include <algorithm>
#include <map>
#include <vector>

namespace
{
    uint8 const SPEC_TAB_UNASSIGNED = 255;
    uint32 const PROFILE_POINT_COUNT = 51;
    uint32 const PROFILE_COUNT = 27;

    enum CatalogState
    {
        CATALOG_NOT_VALIDATED,
        CATALOG_VALID,
        CATALOG_INVALID,
    };

    CatalogState g_catalogState = CATALOG_NOT_VALIDATED;

    struct TalentChunk
    {
        uint16 talentId;
        uint8 count;
    };

    struct TalentProfile
    {
        uint8 classId;
        uint8 specTab;
        char const* name;
        CombatBotRoles defaultRole;
        uint8 allowedRoleMask;
        TalentChunk const* chunks;
        size_t chunkCount;
    };

    typedef std::map<uint32, uint8> TalentRanks;

    uint8 const ROLE_MASK_MELEE = uint8(1u << ROLE_MELEE_DPS);
    uint8 const ROLE_MASK_RANGE = uint8(1u << ROLE_RANGE_DPS);
    uint8 const ROLE_MASK_TANK = uint8(1u << ROLE_TANK);
    uint8 const ROLE_MASK_HEALER = uint8(1u << ROLE_HEALER);

    // Generated from docs/bot-spec-research/talent_profiles.json
    // schema_version=1, client_build=5875. Split chunks are intentional.
    TalentChunk const kWarriorArms[] = { {127, 3}, {130, 3}, {126, 2}, {641, 2}, {131, 2}, {121, 3}, {641, 3}, {137, 1}, {136, 1}, {133, 1}, {132, 5}, {136, 4}, {135, 1}, {157, 5}, {159, 5}, {160, 1}, {154, 4}, {155, 5} };
    TalentChunk const kWarriorFury[] = { {157, 5}, {159, 5}, {160, 1}, {661, 3}, {154, 1}, {155, 5}, {165, 1}, {154, 4}, {156, 5}, {167, 1}, {127, 3}, {130, 2}, {641, 5}, {131, 1}, {137, 1}, {121, 3}, {136, 5} };
    TalentChunk const kWarriorProtection[] = { {1601, 5}, {140, 5}, {145, 1}, {142, 2}, {153, 1}, {147, 1}, {144, 5}, {152, 1}, {147, 2}, {143, 2}, {702, 5}, {148, 1}, {130, 5}, {138, 5}, {641, 5}, {137, 1}, {157, 4} };
    TalentChunk const kPaladinHoly[] = { {1449, 5}, {1432, 5}, {1435, 1}, {1444, 3}, {1443, 1}, {1461, 5}, {1433, 1}, {1446, 2}, {1465, 2}, {1627, 5}, {1502, 1}, {1443, 1}, {1628, 2}, {1465, 1}, {1422, 5}, {1630, 3}, {1425, 2}, {1442, 1}, {1401, 5} };
    TalentChunk const kPaladinProtection[] = { {1450, 5}, {1463, 5}, {1435, 1}, {1421, 5}, {1630, 3}, {1423, 2}, {1442, 1}, {1501, 3}, {1424, 3}, {1423, 3}, {1431, 1}, {1425, 2}, {1521, 2}, {1429, 5}, {1430, 1}, {1407, 5}, {1631, 2}, {1403, 2} };
    TalentChunk const kPaladinRetribution[] = { {1407, 5}, {1631, 2}, {1464, 3}, {1481, 1}, {1411, 5}, {1634, 2}, {1403, 3}, {1409, 1}, {1410, 3}, {1402, 5}, {1441, 1}, {1450, 5}, {1432, 5}, {1435, 1}, {1403, 1}, {1422, 5}, {1630, 3} };
    TalentChunk const kHunterBeastMastery[] = { {1382, 5}, {1395, 3}, {1625, 2}, {1384, 2}, {1391, 1}, {1396, 5}, {1393, 2}, {1387, 1}, {1393, 3}, {1388, 1}, {1397, 5}, {1386, 1}, {1342, 5}, {1344, 5}, {1345, 1}, {1352, 3}, {1341, 1}, {1349, 5} };
    TalentChunk const kHunterMarksmanship[] = { {1342, 5}, {1344, 5}, {1345, 1}, {1352, 3}, {1343, 1}, {1349, 5}, {1353, 1}, {1347, 3}, {1343, 1}, {1362, 5}, {1361, 1}, {1382, 5}, {1625, 2}, {1395, 3}, {1391, 1}, {1396, 5}, {1393, 4} };
    TalentChunk const kHunterSurvival[] = { {1623, 3}, {1301, 3}, {1311, 5}, {1306, 2}, {1622, 5}, {1308, 1}, {1310, 3}, {1321, 3}, {1303, 5}, {1325, 1}, {1342, 5}, {1344, 5}, {1345, 1}, {1352, 3}, {1343, 1}, {1349, 5} };
    TalentChunk const kRogueAssassination[] = { {270, 5}, {276, 3}, {273, 2}, {281, 1}, {269, 5}, {682, 4}, {280, 1}, {682, 1}, {268, 3}, {283, 5}, {382, 1}, {201, 2}, {203, 3}, {202, 3}, {261, 5}, {244, 5}, {247, 2} };
    TalentChunk const kRogueCombat[] = { {201, 2}, {203, 3}, {187, 5}, {301, 1}, {204, 2}, {222, 2}, {181, 5}, {223, 1}, {221, 5}, {1122, 3}, {1703, 1}, {205, 1}, {1703, 1}, {270, 5}, {277, 3}, {274, 2}, {281, 1}, {269, 5}, {273, 3} };
    TalentChunk const kRogueSubtlety[] = { {261, 5}, {244, 5}, {303, 1}, {263, 3}, {247, 1}, {1123, 3}, {262, 2}, {681, 1}, {284, 1}, {262, 1}, {1701, 2}, {1702, 5}, {272, 2}, {270, 5}, {274, 2}, {276, 1}, {281, 1}, {276, 2}, {269, 5}, {682, 2}, {280, 1} };
    TalentChunk const kPriestDiscipline[] = { {465, 5}, {345, 5}, {343, 3}, {344, 2}, {352, 5}, {348, 1}, {347, 3}, {341, 5}, {1201, 5}, {351, 1}, {322, 1}, {410, 2}, {406, 3}, {401, 5}, {1181, 5} };
    TalentChunk const kPriestHoly[] = { {465, 5}, {345, 5}, {410, 2}, {406, 3}, {401, 5}, {1181, 5}, {442, 1}, {361, 3}, {408, 1}, {403, 2}, {408, 2}, {402, 5}, {404, 1}, {343, 3}, {344, 2}, {348, 1}, {347, 3}, {352, 2} };
    TalentChunk const kPriestShadow[] = { {465, 5}, {482, 2}, {463, 3}, {501, 1}, {464, 2}, {542, 2}, {881, 3}, {461, 5}, {541, 1}, {484, 1}, {462, 5}, {521, 1}, {345, 5}, {343, 3}, {344, 2}, {348, 1}, {347, 3}, {352, 1}, {341, 5} };
    TalentChunk const kShamanElemental[] = { {564, 5}, {561, 3}, {563, 2}, {574, 1}, {562, 5}, {1642, 3}, {563, 1}, {565, 1}, {1641, 2}, {563, 2}, {721, 5}, {573, 1}, {586, 5}, {589, 2}, {595, 3}, {583, 3}, {582, 1}, {595, 1}, {594, 5} };
    TalentChunk const kShamanEnhancement[] = { {612, 5}, {613, 5}, {617, 1}, {607, 3}, {601, 1}, {602, 5}, {616, 1}, {611, 3}, {601, 1}, {1643, 5}, {901, 1}, {586, 5}, {589, 2}, {595, 3}, {583, 3}, {582, 1}, {587, 5}, {1646, 1} };
    TalentChunk const kShamanRestoration[] = { {586, 5}, {593, 5}, {581, 3}, {595, 2}, {588, 5}, {591, 1}, {583, 3}, {582, 1}, {592, 5}, {590, 1}, {595, 3}, {587, 5}, {1646, 3}, {1648, 3}, {594, 1}, {614, 5} };
    TalentChunk const kMageArcane[] = { {80, 5}, {78, 2}, {75, 5}, {81, 3}, {85, 1}, {88, 2}, {1142, 2}, {86, 1}, {77, 5}, {421, 3}, {1142, 1}, {87, 1}, {37, 5}, {1649, 3}, {73, 5}, {61, 3}, {69, 1}, {66, 3} };
    TalentChunk const kMageFire[] = { {1649, 3}, {26, 5}, {30, 3}, {34, 2}, {29, 1}, {23, 2}, {34, 3}, {1639, 3}, {28, 1}, {33, 3}, {32, 1}, {28, 1}, {35, 5}, {36, 1}, {74, 2}, {76, 5}, {75, 5}, {81, 3}, {85, 1}, {1142, 1} };
    TalentChunk const kMageFrost[] = { {37, 5}, {38, 3}, {62, 2}, {73, 5}, {67, 5}, {72, 1}, {61, 3}, {69, 1}, {66, 3}, {1649, 2}, {71, 1}, {1649, 1}, {741, 1}, {74, 2}, {76, 4}, {75, 5}, {81, 3}, {85, 1}, {88, 2}, {1142, 1} };
    TalentChunk const kWarlockAffliction[] = { {1003, 5}, {1005, 3}, {1007, 2}, {1101, 2}, {1004, 3}, {1001, 5}, {1041, 1}, {1002, 2}, {1021, 2}, {1042, 5}, {1022, 1}, {1223, 5}, {1225, 3}, {1242, 5}, {1226, 1}, {1241, 4}, {1227, 2} };
    TalentChunk const kWarlockDemonology[] = { {1003, 5}, {1005, 2}, {1223, 5}, {1225, 3}, {1242, 3}, {1226, 1}, {1241, 5}, {1227, 2}, {1262, 5}, {1281, 1}, {1244, 5}, {1282, 1}, {1005, 1}, {1007, 2}, {1101, 2}, {1004, 3}, {1001, 5} };
    TalentChunk const kWarlockDestruction[] = { {941, 5}, {943, 5}, {981, 5}, {963, 1}, {985, 2}, {964, 2}, {967, 1}, {961, 5}, {966, 4}, {968, 1}, {966, 1}, {983, 2}, {1003, 5}, {1005, 3}, {1007, 2}, {1101, 2}, {1284, 3}, {1002, 2} };
    TalentChunk const kDruidBalance[] = { {761, 1}, {921, 4}, {763, 5}, {764, 2}, {762, 3}, {792, 5}, {789, 1}, {783, 3}, {762, 1}, {790, 5}, {793, 1}, {821, 5}, {824, 5}, {827, 1}, {829, 3}, {823, 1}, {843, 5} };
    TalentChunk const kDruidFeralCombat[] = { {796, 5}, {799, 5}, {807, 2}, {822, 4}, {804, 1}, {798, 3}, {801, 2}, {803, 3}, {805, 2}, {1162, 1}, {800, 2}, {808, 5}, {809, 1}, {822, 1}, {761, 1}, {921, 4}, {791, 5}, {788, 1}, {781, 3} };
    TalentChunk const kDruidRestoration[] = { {821, 5}, {824, 5}, {827, 1}, {829, 3}, {823, 1}, {843, 5}, {831, 1}, {828, 5}, {830, 3}, {825, 1}, {844, 1}, {762, 5}, {761, 1}, {921, 4}, {763, 5}, {764, 2}, {784, 3} };

#define PROFILE(classId, specTab, name, role, roles, chunks) \
    { classId, specTab, name, role, roles, chunks, sizeof(chunks) / sizeof(TalentChunk) }

    TalentProfile const kProfiles[] =
    {
        PROFILE(CLASS_WARRIOR, 0, "warrior_arms", ROLE_MELEE_DPS, ROLE_MASK_MELEE | ROLE_MASK_TANK, kWarriorArms),
        PROFILE(CLASS_WARRIOR, 1, "warrior_fury", ROLE_MELEE_DPS, ROLE_MASK_MELEE | ROLE_MASK_TANK, kWarriorFury),
        PROFILE(CLASS_WARRIOR, 2, "warrior_protection", ROLE_TANK, ROLE_MASK_TANK, kWarriorProtection),
        PROFILE(CLASS_PALADIN, 0, "paladin_holy", ROLE_HEALER, ROLE_MASK_HEALER, kPaladinHoly),
        PROFILE(CLASS_PALADIN, 1, "paladin_protection", ROLE_TANK, ROLE_MASK_TANK, kPaladinProtection),
        PROFILE(CLASS_PALADIN, 2, "paladin_retribution", ROLE_MELEE_DPS, ROLE_MASK_MELEE, kPaladinRetribution),
        PROFILE(CLASS_HUNTER, 0, "hunter_beast_mastery", ROLE_RANGE_DPS, ROLE_MASK_RANGE, kHunterBeastMastery),
        PROFILE(CLASS_HUNTER, 1, "hunter_marksmanship", ROLE_RANGE_DPS, ROLE_MASK_RANGE, kHunterMarksmanship),
        PROFILE(CLASS_HUNTER, 2, "hunter_survival", ROLE_RANGE_DPS, ROLE_MASK_RANGE, kHunterSurvival),
        PROFILE(CLASS_ROGUE, 0, "rogue_assassination", ROLE_MELEE_DPS, ROLE_MASK_MELEE, kRogueAssassination),
        PROFILE(CLASS_ROGUE, 1, "rogue_combat", ROLE_MELEE_DPS, ROLE_MASK_MELEE, kRogueCombat),
        PROFILE(CLASS_ROGUE, 2, "rogue_subtlety", ROLE_MELEE_DPS, ROLE_MASK_MELEE, kRogueSubtlety),
        PROFILE(CLASS_PRIEST, 0, "priest_discipline", ROLE_HEALER, ROLE_MASK_HEALER, kPriestDiscipline),
        PROFILE(CLASS_PRIEST, 1, "priest_holy", ROLE_HEALER, ROLE_MASK_HEALER, kPriestHoly),
        PROFILE(CLASS_PRIEST, 2, "priest_shadow", ROLE_RANGE_DPS, ROLE_MASK_RANGE, kPriestShadow),
        PROFILE(CLASS_SHAMAN, 0, "shaman_elemental", ROLE_RANGE_DPS, ROLE_MASK_RANGE, kShamanElemental),
        PROFILE(CLASS_SHAMAN, 1, "shaman_enhancement", ROLE_MELEE_DPS, ROLE_MASK_MELEE, kShamanEnhancement),
        PROFILE(CLASS_SHAMAN, 2, "shaman_restoration", ROLE_HEALER, ROLE_MASK_HEALER, kShamanRestoration),
        PROFILE(CLASS_MAGE, 0, "mage_arcane", ROLE_RANGE_DPS, ROLE_MASK_RANGE, kMageArcane),
        PROFILE(CLASS_MAGE, 1, "mage_fire", ROLE_RANGE_DPS, ROLE_MASK_RANGE, kMageFire),
        PROFILE(CLASS_MAGE, 2, "mage_frost", ROLE_RANGE_DPS, ROLE_MASK_RANGE, kMageFrost),
        PROFILE(CLASS_WARLOCK, 0, "warlock_affliction", ROLE_RANGE_DPS, ROLE_MASK_RANGE, kWarlockAffliction),
        PROFILE(CLASS_WARLOCK, 1, "warlock_demonology", ROLE_RANGE_DPS, ROLE_MASK_RANGE, kWarlockDemonology),
        PROFILE(CLASS_WARLOCK, 2, "warlock_destruction", ROLE_RANGE_DPS, ROLE_MASK_RANGE, kWarlockDestruction),
        PROFILE(CLASS_DRUID, 0, "druid_balance", ROLE_RANGE_DPS, ROLE_MASK_RANGE, kDruidBalance),
        PROFILE(CLASS_DRUID, 1, "druid_feral_combat", ROLE_MELEE_DPS, ROLE_MASK_MELEE | ROLE_MASK_TANK, kDruidFeralCombat),
        PROFILE(CLASS_DRUID, 2, "druid_restoration", ROLE_HEALER, ROLE_MASK_HEALER, kDruidRestoration),
    };

#undef PROFILE

    TalentProfile const* FindProfile(uint8 classId, uint8 specTab)
    {
        for (TalentProfile const& profile : kProfiles)
            if (profile.classId == classId && profile.specTab == specTab)
                return &profile;
        return nullptr;
    }

    void BuildDesiredRanks(TalentProfile const& profile, TalentRanks& ranks)
    {
        ranks.clear();
        for (size_t i = 0; i < profile.chunkCount; ++i)
            ranks[profile.chunks[i].talentId] += profile.chunks[i].count;
    }

    uint32 CaptureActualRanks(Player const* player, TalentRanks& ranks)
    {
        ranks.clear();
        uint32 spent = 0;
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* talent = sTalentStore.LookupEntry(i);
            if (!talent)
                continue;

            for (int32 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            {
                if (talent->RankID[rank] && player->HasSpell(talent->RankID[rank]))
                {
                    ranks[talent->TalentID] = uint8(rank + 1);
                    spent += uint32(rank + 1);
                    break;
                }
            }
        }
        return spent;
    }

    bool IsCompatible(TalentRanks const& actual, TalentProfile const& profile)
    {
        TalentRanks desired;
        BuildDesiredRanks(profile, desired);
        for (TalentRanks::const_iterator itr = actual.begin(); itr != actual.end(); ++itr)
        {
            TalentRanks::const_iterator wanted = desired.find(itr->first);
            if (wanted == desired.end() || itr->second > wanted->second)
                return false;
        }
        return true;
    }

    bool IsAllowedRole(TalentProfile const& profile, CombatBotRoles role)
    {
        return role > ROLE_INVALID && role <= ROLE_HEALER &&
            (profile.allowedRoleMask & uint8(1u << role)) != 0;
    }

    CombatBotRoles DefaultRoleForPlayer(TalentProfile const& profile, uint32 guidLow)
    {
        if (profile.classId == CLASS_DRUID && profile.specTab == 1)
            return ((guidLow / 3) % 2) ? ROLE_TANK : ROLE_MELEE_DPS;
        return profile.defaultRole;
    }

    void PersistMetadata(PlayerBotEntry const* entry)
    {
        if (!entry || !entry->playerGUID)
            return;

        CharacterDatabase.PExecute(
            "UPDATE `playerbot` SET `spec_tab`='%u', `active_role`='%u' WHERE `char_guid`='%u'",
            uint32(entry->specTab), uint32(entry->activeRole), uint32(entry->playerGUID));
    }

    bool SnapshotMatches(Player const* player, AiBotTalents::TalentSnapshot const& snapshot)
    {
        TalentRanks actual;
        CaptureActualRanks(player, actual);
        if (actual.size() != snapshot.talents.size())
            return false;

        for (AiBotTalents::TalentSnapshotEntry const& saved : snapshot.talents)
        {
            TalentRanks::const_iterator itr = actual.find(saved.talentId);
            if (itr == actual.end() || itr->second != saved.rank)
                return false;
        }
        return true;
    }
}

bool AiBotTalents::ValidateProfiles()
{
    if (g_catalogState != CATALOG_NOT_VALIDATED)
        return g_catalogState == CATALOG_VALID;

    if (sizeof(kProfiles) / sizeof(TalentProfile) != PROFILE_COUNT)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-TALENT] manifest has wrong profile count; talent spending disabled");
        g_catalogState = CATALOG_INVALID;
        return false;
    }

    bool seen[MAX_CLASSES][3] = {};
    for (TalentProfile const& profile : kProfiles)
    {
        if (profile.classId == 0 || profile.classId >= MAX_CLASSES || profile.specTab > 2 ||
            seen[profile.classId][profile.specTab])
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-TALENT] invalid or duplicate profile %s; talent spending disabled", profile.name);
            g_catalogState = CATALOG_INVALID;
            return false;
        }
        seen[profile.classId][profile.specTab] = true;

        uint32 points = 0;
        TalentRanks ranks;
        std::map<uint32, uint32> tabPoints;
        for (size_t i = 0; i < profile.chunkCount; ++i)
        {
            TalentChunk const& chunk = profile.chunks[i];
            if (!chunk.count)
            {
                g_catalogState = CATALOG_INVALID;
                return false;
            }

            for (uint8 n = 0; n < chunk.count; ++n)
            {
                TalentEntry const* talent = sTalentStore.LookupEntry(chunk.talentId);
                TalentTabEntry const* tab = talent ? sTalentTabStore.LookupEntry(talent->TalentTab) : nullptr;
                uint8 currentRank = ranks[chunk.talentId];
                if (!talent || !tab || (tab->ClassMask & (1u << (profile.classId - 1))) == 0 ||
                    currentRank >= MAX_TALENT_RANK || !talent->RankID[currentRank] ||
                    tabPoints[talent->TalentTab] < talent->Row * MAX_TALENT_RANK ||
                    (talent->DependsOn && ranks[talent->DependsOn] < talent->DependsOnRank + 1))
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                        "[AIBOT-TALENT] DBC validation failed for profile %s at talent %u rank %u; spending disabled",
                        profile.name, uint32(chunk.talentId), uint32(currentRank + 1));
                    g_catalogState = CATALOG_INVALID;
                    return false;
                }
                ++ranks[chunk.talentId];
                ++tabPoints[talent->TalentTab];
                ++points;
            }
        }

        if (points != PROFILE_POINT_COUNT)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-TALENT] profile %s has %u points instead of 51; spending disabled",
                profile.name, points);
            g_catalogState = CATALOG_INVALID;
            return false;
        }
    }

    g_catalogState = CATALOG_VALID;
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-TALENT] validated 27 build-5875 profiles (1377 ordered purchases)");
    return true;
}

AiBotTalents::RepairResult AiBotTalents::EnsureProfileAndTalents(Player* player, PlayerBotEntry* entry)
{
    RepairResult result;
    if (!player || !entry)
        return result;

    entry->talentProfileState = PB_TALENT_PROFILE_UNCHECKED;

    if (!ValidateProfiles())
    {
        result.status = TALENT_REPAIR_DISABLED;
        entry->talentProfileState = PB_TALENT_PROFILE_DISABLED;
        return result;
    }

    TalentRanks actual;
    uint32 const spentPoints = CaptureActualRanks(player, actual);
    uint32 const targetPoints = player->GetLevel() > 9
        ? std::min<uint32>(player->GetLevel() - 9, PROFILE_POINT_COUNT) : 0;
    if (spentPoints > targetPoints)
    {
        result.status = TALENT_REPAIR_CONFLICT;
        entry->talentProfileState = PB_TALENT_PROFILE_CONFLICT;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-TALENT] %s (guid %u) has %u talent points at level %u (maximum %u); preserving build",
            player->GetName(), player->GetGUIDLow(), spentPoints,
            uint32(player->GetLevel()), targetPoints);
        return result;
    }
    TalentProfile const* profile = nullptr;

    if (entry->specTab == SPEC_TAB_UNASSIGNED)
    {
        if (!spentPoints)
        {
            profile = FindProfile(player->GetClass(), uint8(player->GetGUIDLow() % 3));
        }
        else
        {
            std::vector<TalentProfile const*> compatible;
            for (uint8 specTab = 0; specTab < 3; ++specTab)
            {
                TalentProfile const* candidate = FindProfile(player->GetClass(), specTab);
                if (candidate && IsCompatible(actual, *candidate))
                    compatible.push_back(candidate);
            }
            if (!compatible.empty())
                profile = compatible[player->GetGUIDLow() % compatible.size()];
        }

        if (!profile)
        {
            result.status = TALENT_REPAIR_CONFLICT;
            entry->talentProfileState = PB_TALENT_PROFILE_CONFLICT;
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-TALENT] %s (guid %u) has %u unassigned, incompatible talent points; preserving build",
                player->GetName(), player->GetGUIDLow(), spentPoints);
            return result;
        }

        entry->specTab = profile->specTab;
        result.metadataChanged = true;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TALENT] %s (guid %u) assigned profile %s",
            player->GetName(), player->GetGUIDLow(), profile->name);
    }
    else if (entry->specTab <= 2)
    {
        profile = FindProfile(player->GetClass(), entry->specTab);
    }
    else
    {
        result.status = TALENT_REPAIR_INVALID_PROFILE;
        entry->talentProfileState = PB_TALENT_PROFILE_INVALID;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-TALENT] %s (guid %u) has invalid spec_tab %u; preserving build",
            player->GetName(), player->GetGUIDLow(), uint32(entry->specTab));
        return result;
    }

    if (!profile)
    {
        result.status = TALENT_REPAIR_INVALID_PROFILE;
        entry->talentProfileState = PB_TALENT_PROFILE_INVALID;
        return result;
    }

    if (!IsAllowedRole(*profile, entry->activeRole))
    {
        entry->activeRole = DefaultRoleForPlayer(*profile, player->GetGUIDLow());
        result.metadataChanged = true;
    }
    result.role = entry->activeRole;

    if (result.metadataChanged)
        PersistMetadata(entry);

    if (!IsCompatible(actual, *profile))
    {
        result.status = TALENT_REPAIR_CONFLICT;
        entry->talentProfileState = PB_TALENT_PROFILE_CONFLICT;
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT-TALENT] %s (guid %u) conflicts with profile %s; preserving learned talents",
            player->GetName(), player->GetGUIDLow(), profile->name);
        return result;
    }

    TalentRanks plannedRanks;
    uint32 plannedPoints = 0;
    for (size_t i = 0; i < profile->chunkCount && plannedPoints < targetPoints &&
         spentPoints + result.learnedPoints < targetPoints; ++i)
    {
        TalentChunk const& chunk = profile->chunks[i];
        for (uint8 n = 0; n < chunk.count && plannedPoints < targetPoints &&
             spentPoints + result.learnedPoints < targetPoints; ++n)
        {
            ++plannedPoints;
            uint8 desiredRank = ++plannedRanks[chunk.talentId];
            if (actual[chunk.talentId] >= desiredRank)
                continue;

            if (!player->GetFreeTalentPoints())
            {
                result.status = TALENT_REPAIR_ERROR;
                entry->talentProfileState = PB_TALENT_PROFILE_ERROR;
                sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                    "[AIBOT-TALENT] %s (guid %u) has no free point for profile %s purchase %u/%u; stopped safely",
                    player->GetName(), player->GetGUIDLow(), profile->name,
                    plannedPoints, targetPoints);
                return result;
            }

            if (!player->LearnTalent(chunk.talentId, desiredRank - 1))
            {
                result.status = TALENT_REPAIR_ERROR;
                entry->talentProfileState = PB_TALENT_PROFILE_ERROR;
                sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                    "[AIBOT-TALENT] %s (guid %u) failed profile %s at talent %u rank %u; stopped safely",
                    player->GetName(), player->GetGUIDLow(), profile->name,
                    uint32(chunk.talentId), uint32(desiredRank));
                return result;
            }

            actual[chunk.talentId] = desiredRank;
            ++result.learnedPoints;
        }
    }

    result.status = result.learnedPoints ? TALENT_REPAIR_UPDATED : TALENT_REPAIR_ALIGNED;
    entry->talentProfileState = PB_TALENT_PROFILE_USABLE;
    if (result.learnedPoints)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-TALENT] %s (guid %u) learned %u point(s) for %s; %u free remain",
            player->GetName(), player->GetGUIDLow(), result.learnedPoints,
            profile->name, player->GetFreeTalentPoints());
    }
    return result;
}

bool AiBotTalents::CaptureSnapshot(Player const* player, PlayerBotEntry const* entry, TalentSnapshot& snapshot)
{
    if (!player || !entry)
        return false;

    snapshot.talents.clear();
    TalentRanks ranks;
    CaptureActualRanks(player, ranks);
    snapshot.talents.reserve(ranks.size());
    for (TalentRanks::const_iterator itr = ranks.begin(); itr != ranks.end(); ++itr)
    {
        TalentSnapshotEntry saved;
        saved.talentId = itr->first;
        saved.rank = itr->second;
        snapshot.talents.push_back(saved);
    }
    snapshot.specTab = entry->specTab;
    snapshot.activeRole = entry->activeRole;
    snapshot.profileState = uint8(entry->talentProfileState);
    return true;
}

bool AiBotTalents::RestoreSnapshot(Player* player, PlayerBotEntry* entry, TalentSnapshot const& snapshot)
{
    if (!player || !entry)
        return false;

    // ResetTalents(true) returns false when no points are spent. That is still a
    // successful empty reset, so restoration is verified from the final ranks.
    player->ResetTalents(true);

    std::vector<bool> applied(snapshot.talents.size(), false);
    bool progress = true;
    while (progress)
    {
        progress = false;
        for (size_t i = 0; i < snapshot.talents.size(); ++i)
        {
            if (applied[i])
                continue;
            TalentSnapshotEntry const& saved = snapshot.talents[i];
            if (saved.rank && player->LearnTalent(saved.talentId, saved.rank - 1))
            {
                applied[i] = true;
                progress = true;
            }
        }
    }

    bool const exact = SnapshotMatches(player, snapshot);
    entry->specTab = snapshot.specTab;
    entry->activeRole = snapshot.activeRole;
    entry->talentProfileState = exact
        ? PlayerBotTalentProfileState(snapshot.profileState)
        : PB_TALENT_PROFILE_ERROR;
    PersistMetadata(entry);
    return exact;
}

bool AiBotTalents::IsProfileRoleAllowed(uint8 classId, uint8 specTab, CombatBotRoles role)
{
    TalentProfile const* profile = FindProfile(classId, specTab);
    return profile && IsAllowedRole(*profile, role);
}

CombatBotRoles AiBotTalents::GetDefaultRole(uint8 classId, uint8 specTab, uint32 guidLow)
{
    TalentProfile const* profile = FindProfile(classId, specTab);
    return profile ? DefaultRoleForPlayer(*profile, guidLow) : ROLE_INVALID;
}

AiBotTalents::ApplyResult AiBotTalents::ApplyProfileAndRole(Player* player, PlayerBotEntry* entry,
    uint8 specTab, CombatBotRoles role, bool resetTalents)
{
    ApplyResult result;
    if (!player || !entry)
        return result;

    if (!ValidateProfiles())
    {
        result.status = TALENT_APPLY_DISABLED;
        return result;
    }

    TalentProfile const* profile = FindProfile(player->GetClass(), specTab);
    if (!profile)
    {
        result.status = TALENT_APPLY_INVALID_PROFILE;
        return result;
    }

    if (role == ROLE_INVALID)
        role = DefaultRoleForPlayer(*profile, player->GetGUIDLow());
    if (!IsAllowedRole(*profile, role))
    {
        result.status = TALENT_APPLY_INVALID_ROLE;
        return result;
    }
    result.role = role;

    if (!resetTalents && entry->specTab != specTab)
    {
        result.status = TALENT_APPLY_RESET_REQUIRED;
        return result;
    }

    TalentSnapshot before;
    if (!CaptureSnapshot(player, entry, before))
        return result;
    for (TalentSnapshotEntry const& saved : before.talents)
        result.removedPoints += saved.rank;

    if (!resetTalents)
    {
        entry->specTab = specTab;
        entry->activeRole = role;
        PersistMetadata(entry);
        result.status = TALENT_APPLY_OK;
        result.removedPoints = 0;
        return result;
    }

    result.resetPerformed = true;
    player->ResetTalents(true); // false for an already-empty build is not an error
    entry->specTab = specTab;
    entry->activeRole = role;
    entry->talentProfileState = PB_TALENT_PROFILE_UNCHECKED;

    RepairResult repair = EnsureProfileAndTalents(player, entry);
    result.learnedPoints = repair.learnedPoints;
    result.role = repair.role;
    if ((repair.status == TALENT_REPAIR_ALIGNED || repair.status == TALENT_REPAIR_UPDATED) &&
        entry->talentProfileState == PB_TALENT_PROFILE_USABLE)
    {
        // EnsureProfileAndTalents only persists when it normalizes metadata. This
        // explicit operation always persists the caller's validated selection.
        PersistMetadata(entry);
        result.status = TALENT_APPLY_OK;
        return result;
    }

    result.rollbackSucceeded = RestoreSnapshot(player, entry, before);
    result.status = result.rollbackSucceeded ? TALENT_APPLY_FAILED : TALENT_APPLY_ROLLBACK_FAILED;
    return result;
}

char const* AiBotTalents::GetApplyStatusCode(TalentApplyStatus status)
{
    switch (status)
    {
        case TALENT_APPLY_OK: return "ok";
        case TALENT_APPLY_DISABLED: return "catalog_disabled";
        case TALENT_APPLY_INVALID_PROFILE: return "invalid_profile";
        case TALENT_APPLY_INVALID_ROLE: return "invalid_role";
        case TALENT_APPLY_RESET_REQUIRED: return "reset_required";
        case TALENT_APPLY_ROLLBACK_FAILED: return "rollback_failed";
        default: return "apply_failed";
    }
}

char const* AiBotTalents::GetProfileName(uint8 classId, uint8 specTab)
{
    TalentProfile const* profile = FindProfile(classId, specTab);
    return profile ? profile->name : "unassigned";
}
