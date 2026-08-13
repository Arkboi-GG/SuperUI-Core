/*
 * SuperUI RTS worldstate foundation (R1). See SuiRts.h for the two-gate and
 * threading laws. R2 adds honor/hero mechanics, R3 territory, R4 dungeons -
 * each fills the blocks this file already frames on the wire.
 */

#include "SuiRts.h"
#include "SuiPossess.h"

#include <atomic>
#include <cstdlib>
#include <unordered_map>

#include "Chat.h"
#include "Database/DatabaseEnv.h"
#include "ObjectMgr.h"
#include "Objects/Player.h"
#include "Server/WorldSession.h"
#include "World.h"

namespace SuiRts
{

// state -----------------------------------------------------------------------

static std::unordered_map<std::string, std::string> s_kv;
static std::atomic<int64> s_honorPool[2];
static std::atomic<bool> s_poolDirty;
static int64 s_botCap[2] = { -1, -1 };
static bool s_honorEnabled = false;
static bool s_heroesEnabled = false;
static bool s_territoryEnabled = false;
static bool s_dungeonsEnabled = false;
static uint32 s_flushMs = 30000;
static uint32 s_flushTimer = 0;

// ruleset scalars -------------------------------------------------------------

std::string GetKV(std::string const& key, std::string const& def)
{
    auto itr = s_kv.find(key);
    return itr != s_kv.end() ? itr->second : def;
}

float GetKVFloat(std::string const& key, float def)
{
    auto itr = s_kv.find(key);
    return itr != s_kv.end() ? float(atof(itr->second.c_str())) : def;
}

int64 GetKVInt(std::string const& key, int64 def)
{
    auto itr = s_kv.find(key);
    return itr != s_kv.end() ? int64(strtoll(itr->second.c_str(), nullptr, 10)) : def;
}

// boot ------------------------------------------------------------------------

static void CreateTables()
{
    // Idempotent, characters DB: vanilla DBs get empty tables and boot clean;
    // config ROWS only ship inside an RTS save (the swap tooling writes them).
    static char const* DDL[] =
    {
        "CREATE TABLE IF NOT EXISTS `superui_rules_zone` ("
        "`zone_id` INT UNSIGNED NOT NULL PRIMARY KEY,"
        "`ore` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`skins` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`herbs` TINYINT UNSIGNED NOT NULL DEFAULT 0)",

        "CREATE TABLE IF NOT EXISTS `superui_rules_hub` ("
        "`hub_id` SMALLINT UNSIGNED NOT NULL PRIMARY KEY,"
        "`zone_id` INT UNSIGNED NOT NULL,"
        "`name` VARCHAR(64) NOT NULL,"
        "`banner_go_guid` INT UNSIGNED NOT NULL,"
        "`event_alliance` SMALLINT UNSIGNED NOT NULL,"
        "`event_horde` SMALLINT UNSIGNED NOT NULL,"
        "`capture_ms` INT UNSIGNED NOT NULL DEFAULT 60000,"
        "`initial_controller` TINYINT UNSIGNED NOT NULL DEFAULT 0)",

        "CREATE TABLE IF NOT EXISTS `superui_rules_hero` ("
        "`hero_level` TINYINT UNSIGNED NOT NULL PRIMARY KEY,"
        "`declare_cost` INT UNSIGNED NOT NULL,"
        "`revive_fee` INT UNSIGNED NOT NULL,"
        "`spell_id` INT UNSIGNED NOT NULL)",

        "CREATE TABLE IF NOT EXISTS `superui_rules_dungeon` ("
        "`map_id` INT UNSIGNED NOT NULL PRIMARY KEY,"
        "`final_boss_entry` INT UNSIGNED NOT NULL,"
        "`buff_spell_id` INT UNSIGNED NOT NULL,"
        "`loot_items` TINYINT UNSIGNED NOT NULL DEFAULT 10)",

        "CREATE TABLE IF NOT EXISTS `superui_faction` ("
        "`team` TINYINT UNSIGNED NOT NULL PRIMARY KEY,"
        "`honor_pool` BIGINT NOT NULL DEFAULT 0)",

        "CREATE TABLE IF NOT EXISTS `superui_heroes` ("
        "`guid` INT UNSIGNED NOT NULL PRIMARY KEY,"
        "`team` TINYINT UNSIGNED NOT NULL,"
        "`hero_level` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
        "`dead` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`declared_at` BIGINT UNSIGNED NOT NULL DEFAULT 0)",

        "CREATE TABLE IF NOT EXISTS `superui_zone_control` ("
        "`zone_id` INT UNSIGNED NOT NULL PRIMARY KEY,"
        "`controller` TINYINT UNSIGNED NOT NULL DEFAULT 0)",

        "CREATE TABLE IF NOT EXISTS `superui_dungeon_control` ("
        "`map_id` INT UNSIGNED NOT NULL PRIMARY KEY,"
        "`controller` TINYINT UNSIGNED NOT NULL DEFAULT 0)",
    };
    for (char const* sql : DDL)
        CharacterDatabase.DirectExecute(sql);
    CharacterDatabase.DirectExecute("INSERT IGNORE INTO `superui_faction` VALUES (0,0),(1,0)");
}

static uint32 CountRows(char const* table)
{
    if (auto result = CharacterDatabase.PQuery("SELECT COUNT(*) FROM `%s`", table))
        return result->Fetch()[0].GetUInt32();
    return 0;
}

static void ApplyRates()
{
    // The stock rate machinery reads sWorld config live at XP-gain / loot-roll
    // time (verified), so a post-LoadConfigSettings override simply sticks.
    // Absent key = mangosd.conf value stands (module rule). Creature HP/damage
    // rates apply at spawn time only - those stay conf-side, documented.
    static struct { char const* key; eConfigFloatValues index; } const RATES[] =
    {
        { "rate.xp_kill",              CONFIG_FLOAT_RATE_XP_KILL },
        { "rate.xp_kill_elite",        CONFIG_FLOAT_RATE_XP_KILL_ELITE },
        { "rate.xp_quest",             CONFIG_FLOAT_RATE_XP_QUEST },
        { "rate.drop_money",           CONFIG_FLOAT_RATE_DROP_MONEY },
        { "rate.drop_item_poor",       CONFIG_FLOAT_RATE_DROP_ITEM_POOR },
        { "rate.drop_item_normal",     CONFIG_FLOAT_RATE_DROP_ITEM_NORMAL },
        { "rate.drop_item_uncommon",   CONFIG_FLOAT_RATE_DROP_ITEM_UNCOMMON },
        { "rate.drop_item_rare",       CONFIG_FLOAT_RATE_DROP_ITEM_RARE },
        { "rate.drop_item_epic",       CONFIG_FLOAT_RATE_DROP_ITEM_EPIC },
        { "rate.drop_item_legendary",  CONFIG_FLOAT_RATE_DROP_ITEM_LEGENDARY },
        { "rate.drop_item_artifact",   CONFIG_FLOAT_RATE_DROP_ITEM_ARTIFACT },
        { "rate.drop_item_referenced", CONFIG_FLOAT_RATE_DROP_ITEM_REFERENCED },
    };
    for (auto const& r : RATES)
    {
        auto itr = s_kv.find(r.key);
        if (itr == s_kv.end())
            continue;
        float value = float(atof(itr->second.c_str()));
        if (value <= 0.0f)
            continue;
        sWorld.setConfig(r.index, value);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[SUI-RTS] rate override: %s = %.2f", r.key, value);
    }
}

void LoadRuleset()
{
    s_kv.clear();
    s_honorPool[0].store(0);
    s_honorPool[1].store(0);
    s_poolDirty.store(false);
    s_botCap[0] = s_botCap[1] = -1;
    s_honorEnabled = s_heroesEnabled = s_territoryEnabled = s_dungeonsEnabled = false;
    s_flushTimer = 0;

    CreateTables();

    if (!SuiPossess::RtsWorldState())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[SUI-RTS] vanilla worldstate: all tier-2 modules inert");
        return;
    }

    if (auto result = CharacterDatabase.Query("SELECT `key`, `value` FROM `superui_worldstate`"))
    {
        do
        {
            Field* fields = result->Fetch();
            s_kv[fields[0].GetCppString()] = fields[1].GetCppString();
        } while (result->NextRow());
    }

    s_flushMs = uint32(GetKVInt("state.flush_ms", 30000));
    ApplyRates();
    s_botCap[0] = GetKVInt("bots.cap.alliance", -1);
    s_botCap[1] = GetKVInt("bots.cap.horde", -1);

    // Module flags = config presence at boot (the module rule).
    for (auto const& kv : s_kv)
        if (kv.first.rfind("honor.weight.", 0) == 0)
        {
            s_honorEnabled = true;
            break;
        }
    s_heroesEnabled    = CountRows("superui_rules_hero") > 0;
    s_territoryEnabled = CountRows("superui_rules_hub") > 0;
    s_dungeonsEnabled  = CountRows("superui_rules_dungeon") > 0;

    if (auto result = CharacterDatabase.Query("SELECT `team`, `honor_pool` FROM `superui_faction`"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 team = uint8(fields[0].GetUInt32());
            if (team < 2)
                s_honorPool[team].store(fields[1].GetInt64());
        } while (result->NextRow());
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[SUI-RTS] ruleset loaded: %u scalars; modules honor=%u heroes=%u territory=%u dungeons=%u; "
        "bot caps A=%ld H=%ld; pools A=%ld H=%ld",
        uint32(s_kv.size()), s_honorEnabled, s_heroesEnabled, s_territoryEnabled, s_dungeonsEnabled,
        long(s_botCap[0]), long(s_botCap[1]),
        long(s_honorPool[0].load()), long(s_honorPool[1].load()));
}

void Reload()
{
    LoadRuleset();
}

// tick / persistence (main thread) --------------------------------------------

static void FlushState(bool direct)
{
    if (!s_poolDirty.exchange(false))
        return;
    for (uint8 t = 0; t < 2; ++t)
    {
        long pool = long(s_honorPool[t].load(std::memory_order_relaxed));
        if (direct)
            CharacterDatabase.DirectPExecute("REPLACE INTO `superui_faction` VALUES (%u, %ld)", t, pool);
        else
            CharacterDatabase.PExecute("REPLACE INTO `superui_faction` VALUES (%u, %ld)", t, pool);
    }
}

void Tick(uint32 diff)
{
    if (!SuiPossess::RtsWorldState())
        return;
    s_flushTimer += diff;
    if (s_flushTimer >= s_flushMs)
    {
        s_flushTimer = 0;
        FlushState(false);
    }
    // R2+: structural action queue (zone flips, buffs, hero mutations) drains here.
}

void Shutdown()
{
    if (SuiPossess::RtsWorldState())
        FlushState(true);
}

// accessors -------------------------------------------------------------------

bool HonorEnabled() { return s_honorEnabled; }
bool HeroesEnabled() { return s_heroesEnabled; }
bool TerritoryEnabled() { return s_territoryEnabled; }
bool DungeonsEnabled() { return s_dungeonsEnabled; }

int64 HonorPool(uint8 teamIdx)
{
    return s_honorPool[teamIdx & 1].load(std::memory_order_relaxed);
}

void AddHonor(uint8 teamIdx, int64 amount)
{
    s_honorPool[teamIdx & 1].fetch_add(amount, std::memory_order_relaxed);
    s_poolDirty.store(true, std::memory_order_relaxed);
}

int64 BotCap(uint8 teamIdx)
{
    return SuiPossess::RtsWorldState() ? s_botCap[teamIdx & 1] : -1;
}

// wire ------------------------------------------------------------------------

void HandleRtsState(WorldSession* session, uint8 /*flags*/)
{
    session->SetSuiCapable(true);

    bool rts = SuiPossess::RtsWorldState();
    uint8 modules = 0;
    if (rts)
    {
        if (s_honorEnabled)     modules |= 0x01;
        if (s_heroesEnabled)    modules |= 0x02;
        if (s_territoryEnabled) modules |= 0x04;
        if (s_dungeonsEnabled)  modules |= 0x08;
    }

    // Stride-versioned blocks: later phases append per-row bytes by bumping the
    // stride; old clients skip the excess (same convention as zone intel).
    WorldPacket data(SMSG_SUI_RTS_STATE, 64);
    data << uint8(rts ? 1 : 0);       // mode
    data << modules;
    data << uint8(26);                // faction row stride
    for (uint8 t = 0; t < 2; ++t)
    {
        data << uint64(rts ? uint64(HonorPool(t)) : 0);   // i64 honor pool
        data << uint32(0);            // ore   - R3
        data << uint32(0);            // skins - R3
        data << uint32(0);            // herbs - R3
        data << uint16(0);            // controlled zones - R3
        data << uint16(0);            // heroes fielded - R2
        data << uint16(0);            // hero slot cap - R2
    }
    data << uint8(0) << uint8(12);    // hero rows - R2
    data << uint8(0) << uint8(7);     // dungeon rows - R4
    session->SendPacket(&data);
}

void HandleRtsAction(WorldSession* session, uint8 action, uint64 subjectGuid)
{
    session->SetSuiCapable(true);

    // Result codes: 0 ok, 1 insufficient honor, 2 no free slot, 3 bad subject,
    // 4 unsupported/disabled. R1 implements nothing yet - R2 fills this in.
    WorldPacket data(SMSG_SUI_RTS_ACTION_RESULT, 1 + 1 + 8 + 8);
    data << action;
    data << uint8(4);
    data << uint64(subjectGuid);
    Player* player = session->GetPlayer();
    uint8 teamIdx = (player && player->GetTeam() == HORDE) ? 1 : 0;
    data << uint64(HonorPool(teamIdx));
    session->SendPacket(&data);
}

} // namespace SuiRts

// thin opcode bodies ----------------------------------------------------------

void WorldSession::HandleSuiRtsStateOpcode(WorldPackets::SuiRts::RtsState const& packet)
{
    SuiRts::HandleRtsState(this, packet.flags);
}

void WorldSession::HandleSuiRtsActionOpcode(WorldPackets::SuiRts::RtsAction const& packet)
{
    SuiRts::HandleRtsAction(this, packet.action, packet.subjectGuid);
}

// GM: .sui rts [status|reload] ------------------------------------------------

bool ChatHandler::HandleSuiRtsCommand(char* args)
{
    if (char* sub = ExtractLiteralArg(&args))
    {
        std::string s = sub;
        if (s == "reload")
        {
            SuiRts::Reload();
            SendSysMessage("SUI RTS ruleset reloaded (runtime only; boot is authoritative).");
            return true;
        }
        if (s != "status")
        {
            SendSysMessage("Usage: .sui rts [status|reload]");
            return false;
        }
    }
    PSendSysMessage("SUI RTS worldstate: %s", SuiPossess::RtsWorldState() ? "RTS MATCH" : "vanilla");
    PSendSysMessage("  modules: honor=%u heroes=%u territory=%u dungeons=%u",
        SuiRts::HonorEnabled(), SuiRts::HeroesEnabled(), SuiRts::TerritoryEnabled(), SuiRts::DungeonsEnabled());
    PSendSysMessage("  honor pools: alliance=%ld horde=%ld",
        long(SuiRts::HonorPool(0)), long(SuiRts::HonorPool(1)));
    PSendSysMessage("  bot caps: alliance=%ld horde=%ld (-1 = uncapped)",
        long(SuiRts::BotCap(0)), long(SuiRts::BotCap(1)));
    PSendSysMessage("  rate.xp_kill now %.2f", sWorld.getConfig(CONFIG_FLOAT_RATE_XP_KILL));
    return true;
}
