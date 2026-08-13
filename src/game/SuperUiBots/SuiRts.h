/*
 * SuperUI RTS worldstate - the tier-2 match layer foundation (phase R1).
 *
 * TWO-GATE LAW (binding): every tier-2 mechanic checks
 *   1. SuiPossess::RtsWorldState()   - the loaded save carries match rules
 *   2. its module Enabled() flag     - the module CONFIG existed at boot
 * Config absent = module inert even in RTS mode; a vanilla DB boots clean.
 *
 * THREADING LAW: kill hooks / loot fill / GO use / repop run on PARALLEL MAP
 * THREADS - they may only do atomic adds or enqueue actions. All structural
 * mutation happens in Tick(), called from World::Update in the post-map-join
 * window (beside sZoneScriptMgr.Update). DB writes are write-behind from the
 * main thread only.
 *
 * The ruleset is BOOT-TIME DATA in the characters DB (it travels with the
 * save the web app swaps in): scalars in superui_worldstate key/value rows,
 * list config in sibling superui_rules_* tables, runtime state in
 * superui_faction / superui_heroes / superui_zone_control /
 * superui_dungeon_control. All DDL is idempotent and core-owned.
 */

#ifndef MANGOS_SUI_RTS_H
#define MANGOS_SUI_RTS_H

#include "Common.h"

#include <string>

class WorldSession;

namespace SuiRts
{
    // lifecycle
    void LoadRuleset();          // boot: right after SuiPossess::LoadWorldState()
    void Reload();               // GM `.sui rts reload` - runtime only, boot is authoritative
    void Tick(uint32 diff);      // World::Update, main thread, post-map-join window
    void Shutdown();             // World::Shutdown - synchronous state flush

    // module flags (config presence at boot)
    bool HonorEnabled();
    bool HeroesEnabled();
    bool TerritoryEnabled();
    bool DungeonsEnabled();

    // ruleset scalars
    std::string GetKV(std::string const& key, std::string const& def);
    float GetKVFloat(std::string const& key, float def);
    int64 GetKVInt(std::string const& key, int64 def);

    // faction state (teamIdx: 0 alliance, 1 horde)
    int64 HonorPool(uint8 teamIdx);
    void AddHonor(uint8 teamIdx, int64 amount);   // atomic; safe from map threads

    /// Per-faction bot population cap from the ruleset; -1 = uncapped (always
    /// -1 outside the RTS worldstate). PlayerBotMgr::AddBot enforces it.
    int64 BotCap(uint8 teamIdx);

    // wire (both PACKET_PROCESS_WORLD = main thread)
    void HandleRtsState(WorldSession* session, uint8 flags);
    void HandleRtsAction(WorldSession* session, uint8 action, uint64 subjectGuid);
}

#endif
