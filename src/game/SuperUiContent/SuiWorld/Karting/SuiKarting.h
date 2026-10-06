/*
 * WoW Karting - the Core half of the opt-in `wow-karting` World Content Pack
 * (MSUIClient shared_docs/WOW_KARTING.md).
 *
 * Every rule here is keyed by PACK DATA: a kart is a mount spell in the pack spell
 * range that carries the kart marker. A server without the pack enabled has no such
 * spell, so it behaves exactly like stock.
 */

#ifndef MANGOS_SUI_KARTING_H
#define MANGOS_SUI_KARTING_H

#include "Common.h"

class Unit;
class AiBotAI;
struct SpellEntry;

namespace SuiKarting
{
    // World Content Pack spells, [38000, 40000) (MangosSuperUI WorldPackContent.SpellIdBase/
    // SpellIdCeiling). Spell ids are uint16 on the 1.12 wire and the Spell Creator owns 40000+.
    constexpr uint32 PACK_SPELL_FIRST = 38000;
    constexpr uint32 PACK_SPELL_END = 40000;

    // A kart spell marks itself with a SPELL_AURA_DUMMY effect whose misc value is 'KART'.
    constexpr int32 KART_MARKER = 0x4B415254;

    bool IsPackSpell(uint32 spellId);

    // A kart: a pack spell that mounts (aura 78) and carries the kart marker.
    bool IsKartSpell(SpellEntry const* spell);

    // Owner rule (2026-10-04): a kart grants NEUTRAL status - a hostile creature does not
    // aggro on a kart rider (players and SuperUI bots alike). It is not god mode: falls,
    // water, players and anything already fighting the rider stay dangerous.
    bool IsNeutralKartRider(Unit const* unit);

    // The race (SuiKartingRace.cpp): one server-authoritative race on the pack's course
    // (game_tele rows kart:<course>:...). Tick runs on the world thread after the map update;
    // TickRacer at the top of a racer bot's AiBotAI::UpdateAI (true = the race owns this tick).
    void Tick(uint32 diff);
    bool TickRacer(AiBotAI* ai, uint32 diff);
    void Shutdown();
}

// The race marshal's gossip (creature_template.script_name = npc_sui_kart_marshal); called from AddScripts().
void AddSC_sui_karting();

#endif
