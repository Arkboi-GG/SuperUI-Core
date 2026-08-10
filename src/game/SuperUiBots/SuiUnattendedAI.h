/*
 * SUI unattended-character AI (CRPG/RTS mode M5).
 *
 * Attached to the REAL player's character while the human drives a possessed
 * bot or roams the free camera. Deliberately a marker subclass of PartyBotAI —
 * the full CombatBotBase party behaviour (follow the anchor, assist, class
 * rotations, eat/drink) with the spawn-time init REPLACED: PartyBotAI's
 * !m_initialized block would teleport, full-heal, and re-equip the character,
 * which is only correct for a freshly fabricated bot. The constructor performs
 * the safe subset (role + spell data) and pre-marks the AI initialized.
 *
 * What this AI can never do, by construction:
 *  - vendor/autosell: PartyBotAI has no vendor behaviour at all, and the
 *    bridge SELL_ITEMS guard refuses real-account characters as a second wall;
 *  - auto-equip / trade auto-accept: those live in OnPacketReceived, which
 *    only fires for socket-less sessions — a real client session bypasses it.
 *
 * The dummy PlayerBotEntry absorbs PartyBotAI's `botEntry->requestRemoval`
 * writes when no valid anchor resolves (never registered with PlayerBotMgr, so
 * the request is inert and the character simply idles).
 */

#ifndef MANGOS_SUI_UNATTENDED_AI_H
#define MANGOS_SUI_UNATTENDED_AI_H

#include "PartyBotAI.h"
#include "PlayerBotMgr.h"

#include <memory>

class SuiUnattendedAI final : public PartyBotAI
{
public:
    /// pAnchor: whom to follow/assist — the possessed bot, else the group leader.
    SuiUnattendedAI(Player* pOwner, Player* pAnchor)
        : PartyBotAI(pAnchor ? pAnchor : pOwner,
                     pOwner->GetMapId(), pOwner->GetInstanceId(),
                     pOwner->GetPositionX(), pOwner->GetPositionY(),
                     pOwner->GetPositionZ(), pOwner->GetOrientation()),
          m_dummyEntry(std::make_unique<PlayerBotEntry>())
    {
        SetPlayer(pOwner);
        botEntry = m_dummyEntry.get();
        AutoAssignRole();
        ResetSpellData();
        PopulateSpellData();
        m_initialized = true;      // skip the fabricated-bot init wholesale
        m_updateTimer.Reset(1000);
    }

    /// Re-point the follow/assist anchor (e.g. the human switched bots).
    void SetAnchor(Player* pAnchor) { if (pAnchor) m_leaderGuid = pAnchor->GetObjectGuid(); }

private:
    std::unique_ptr<PlayerBotEntry> m_dummyEntry;
};

#endif
