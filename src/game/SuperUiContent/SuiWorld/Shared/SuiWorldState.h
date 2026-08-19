/*
 * SuperUI shared boot-latched worldstate.
 */

#ifndef MANGOS_SUI_WORLD_STATE_H
#define MANGOS_SUI_WORLD_STATE_H

namespace SuiWorldState
{
    // ── Worldstate (vanilla vs RTS match) ────────────────────────────────────
    // TWO TIERS, owner rule 2026-08-12. Tier 1 — everything vanilla-valid
    // (possession, free view, orders, links, commander map + census) — is
    // ALWAYS available, in the normal world too, and must never key on this.
    // Tier 2 — match mechanics that fundamentally change the game (XP scaling,
    // hub captures, hero units, non-respawning commanders) — is INERT unless
    // the loaded save says otherwise: the characters DB carries a
    // superui_worldstate row (key=mode, value=rts) INSIDE the snapshot the
    // MangosSuperUI web app swaps in, so loading the RTS world brings its own
    // rules and the vanilla world can never accidentally run them.
    // BINDING RULE: no tier-2 mechanic ships without a RtsWorldState() gate.
    void LoadWorldState();                 // boot-time read (World::SetInitialWorldSettings)
    bool RtsWorldState();
}

#endif
