/*
 * SuperUI RTS faction Honor (R2).
 *
 * Kill hooks run on map threads.  This module therefore keeps its configured
 * weights in atomics and delegates the actual pool mutation to SuiRts::AddHonor.
 */

#ifndef MANGOS_SUI_HONOR_H
#define MANGOS_SUI_HONOR_H

class Player;
class Unit;

namespace SuiHonor
{
    /// Refresh the boot-time scalar cache after SuiRts has loaded its KV rows.
    void LoadRuleset();

    /// The single faction-Honor kill seam. Safe to call from a map thread.
    void OnUnitKill(Unit* killer, Unit* victim);

    /// True only for a bot recipient receiving vanilla HK credit for a bot
    /// victim. Human recipients are deliberately never suppressed.
    bool SuppressVanillaHonor(Player const* recipient, Player const* victim);
}

#endif
