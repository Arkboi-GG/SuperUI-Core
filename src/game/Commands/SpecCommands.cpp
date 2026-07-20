/*
 * Dual-spec TEST HARNESS — throwaway scaffolding, not production code.
 *
 * Commands:
 *   .testbars <button> <spellId>   push one button + resend the whole bar (redraw test)
 *   .spec save <n>                 snapshot current talents + action bars into spec n
 *   .spec show <n>                 print what is stored for spec n (applies nothing)
 *   .spec load <n>                 ResetTalents(true) -> replay talents -> restore bars
 *   .spec bars <n>                 restore ONLY the bars for spec n (race workaround)
 *
 * Scratch tables are created on first use in the CHARACTERS database:
 *   character_spec_test          (guid, spec, talent_id, rank)
 *   character_spec_action_test   (guid, spec, button, action, type)
 *
 * Drop both tables when this harness is retired.
 */

#include "Chat.h"
#include "Language.h"
#include "Player.h"
#include "MasterPlayer.h"
#include "WorldSession.h"
#include "ObjectGuid.h"
#include "Database/DatabaseEnv.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Log.h"

#include <vector>
#include <map>

namespace
{
    struct StoredTalent
    {
        uint32 talentId;
        uint32 rank;
    };

    struct StoredButton
    {
        uint8  button;
        uint32 action;
        uint8  type;
    };

    // Created lazily so the harness needs no manual DDL step.
    void EnsureSpecTables()
    {
        static bool s_done = false;
        if (s_done)
            return;

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `character_spec_test` ("
            "`guid` INT UNSIGNED NOT NULL,"
            "`spec` TINYINT UNSIGNED NOT NULL,"
            "`talent_id` INT UNSIGNED NOT NULL,"
            "`rank` TINYINT UNSIGNED NOT NULL,"
            "PRIMARY KEY (`guid`,`spec`,`talent_id`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8");

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `character_spec_action_test` ("
            "`guid` INT UNSIGNED NOT NULL,"
            "`spec` TINYINT UNSIGNED NOT NULL,"
            "`button` TINYINT UNSIGNED NOT NULL,"
            "`action` INT UNSIGNED NOT NULL,"
            "`type` TINYINT UNSIGNED NOT NULL,"
            "PRIMARY KEY (`guid`,`spec`,`button`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8");

        s_done = true;
    }

    // Inverse of the curtalent_maxrank block inside Player::LearnTalent:
    // for every talent of this character's class, find the highest known rank.
    void CaptureTalents(Player* player, std::vector<StoredTalent>& out)
    {
        uint32 const numRows = sTalentStore.GetNumRows();

        for (uint32 i = 0; i < numRows; ++i)
        {
            TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);
            if (!talentInfo)
                continue;

            TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
            if (!talentTabInfo)
                continue;

            if ((player->GetClassMask() & talentTabInfo->ClassMask) == 0)
                continue;

            for (int32 k = MAX_TALENT_RANK - 1; k > -1; --k)
            {
                if (talentInfo->RankID[k] && player->HasSpell(talentInfo->RankID[k]))
                {
                    StoredTalent st;
                    st.talentId = i;
                    st.rank     = uint32(k);
                    out.push_back(st);
                    break;
                }
            }
        }
    }

    // Row-gate safe: LearnTalent rejects a talent until enough points are spent
    // in its tab, so a single ordered pass silently drops the deep talents.
    // Repeat until a full pass applies nothing, then report the leftovers.
    uint32 ReplayTalents(Player* player, std::vector<StoredTalent> const& stored, uint32& outFailed)
    {
        std::vector<bool> applied(stored.size(), false);
        uint32 total = 0;

        bool progress = true;
        while (progress)
        {
            progress = false;

            for (size_t i = 0; i < stored.size(); ++i)
            {
                if (applied[i])
                    continue;

                if (player->LearnTalent(stored[i].talentId, stored[i].rank))
                {
                    applied[i] = true;
                    progress = true;
                    ++total;
                }
            }
        }

        outFailed = 0;
        for (size_t i = 0; i < applied.size(); ++i)
            if (!applied[i])
                ++outFailed;

        return total;
    }

    void LoadStoredTalents(uint32 guidLow, uint32 spec, std::vector<StoredTalent>& out)
    {
        std::unique_ptr<QueryResult> result(CharacterDatabase.PQuery(
            "SELECT `talent_id`, `rank` FROM `character_spec_test` WHERE `guid` = %u AND `spec` = %u",
            guidLow, spec));

        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            StoredTalent st;
            st.talentId = fields[0].GetUInt32();
            st.rank     = fields[1].GetUInt32();
            out.push_back(st);
        }
        while (result->NextRow());
    }

    void LoadStoredButtons(uint32 guidLow, uint32 spec, std::vector<StoredButton>& out)
    {
        std::unique_ptr<QueryResult> result(CharacterDatabase.PQuery(
            "SELECT `button`, `action`, `type` FROM `character_spec_action_test` "
            "WHERE `guid` = %u AND `spec` = %u ORDER BY `button`",
            guidLow, spec));

        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            StoredButton sb;
            sb.button = fields[0].GetUInt8();
            sb.action = fields[1].GetUInt32();
            sb.type   = fields[2].GetUInt8();
            out.push_back(sb);
        }
        while (result->NextRow());
    }

    // Wipe the live bar, lay the stored one down, push it to the client.
    // addActionButton passes nullptr to IsActionButtonDataValid, so buttons for
    // spells the character does not know yet are still accepted.
    uint32 RestoreButtons(MasterPlayer* master, std::vector<StoredButton> const& stored)
    {
        ActionButtonList& buttons = master->GetActionButtons();

        std::vector<uint8> existing;
        for (ActionButtonList::const_iterator itr = buttons.begin(); itr != buttons.end(); ++itr)
            existing.push_back(itr->first);

        for (size_t i = 0; i < existing.size(); ++i)
            master->removeActionButton(existing[i]);

        uint32 placed = 0;
        for (size_t i = 0; i < stored.size(); ++i)
            if (master->addActionButton(stored[i].button, stored[i].action, stored[i].type))
                ++placed;

        master->SendInitialActionButtons();
        return placed;
    }
}

// ############################################################################
// .testbars <button> <spellId>
// ############################################################################
bool ChatHandler::HandleTestBarsCommand(char* args)
{
    Player* player = m_session ? m_session->GetPlayer() : nullptr;
    if (!player)
    {
        SendSysMessage("This command must be run in-game.");
        SetSentErrorMessage(true);
        return false;
    }

    MasterPlayer* master = m_session->GetMasterPlayer();
    if (!master)
    {
        SendSysMessage("No MasterPlayer on this session.");
        SetSentErrorMessage(true);
        return false;
    }

    uint32 button = 0;
    uint32 spellId = 0;

    if (!ExtractUInt32(&args, button) || !ExtractUInt32(&args, spellId))
    {
        SendSysMessage("Syntax: .testbars <button 0-119> <spellId>");
        SetSentErrorMessage(true);
        return false;
    }

    if (button >= MAX_ACTION_BUTTONS)
    {
        PSendSysMessage("Button must be 0..%u.", uint32(MAX_ACTION_BUTTONS - 1));
        SetSentErrorMessage(true);
        return false;
    }

    if (!master->addActionButton(uint8(button), spellId, ACTION_BUTTON_SPELL))
    {
        SendSysMessage("addActionButton refused that button/action pair.");
        SetSentErrorMessage(true);
        return false;
    }

    master->SendInitialActionButtons();

    PSendSysMessage("Set button %u to spell %u and resent SMSG_ACTION_BUTTONS. "
                    "If the bar changed without a relog, mid-session redraw works.",
                    button, spellId);
    return true;
}

// ############################################################################
// .spec save <n>
// ############################################################################
bool ChatHandler::HandleSpecSaveCommand(char* args)
{
    Player* player = m_session ? m_session->GetPlayer() : nullptr;
    if (!player)
    {
        SendSysMessage("This command must be run in-game.");
        SetSentErrorMessage(true);
        return false;
    }

    MasterPlayer* master = m_session->GetMasterPlayer();
    if (!master)
    {
        SendSysMessage("No MasterPlayer on this session.");
        SetSentErrorMessage(true);
        return false;
    }

    uint32 spec = 0;
    if (!ExtractUInt32(&args, spec))
    {
        SendSysMessage("Syntax: .spec save <n>");
        SetSentErrorMessage(true);
        return false;
    }

    EnsureSpecTables();

    uint32 const guidLow = player->GetGUIDLow();

    std::vector<StoredTalent> talents;
    CaptureTalents(player, talents);

    ActionButtonList& buttons = master->GetActionButtons();

    CharacterDatabase.BeginTransaction(guidLow);

    CharacterDatabase.PExecute("DELETE FROM `character_spec_test` WHERE `guid` = %u AND `spec` = %u",
                               guidLow, spec);
    CharacterDatabase.PExecute("DELETE FROM `character_spec_action_test` WHERE `guid` = %u AND `spec` = %u",
                               guidLow, spec);

    for (size_t i = 0; i < talents.size(); ++i)
    {
        CharacterDatabase.PExecute(
            "INSERT INTO `character_spec_test` (`guid`,`spec`,`talent_id`,`rank`) VALUES (%u, %u, %u, %u)",
            guidLow, spec, talents[i].talentId, talents[i].rank);
    }

    uint32 buttonCount = 0;
    for (ActionButtonList::const_iterator itr = buttons.begin(); itr != buttons.end(); ++itr)
    {
        if (itr->second.uState == ACTIONBUTTON_DELETED)
            continue;

        CharacterDatabase.PExecute(
            "INSERT INTO `character_spec_action_test` (`guid`,`spec`,`button`,`action`,`type`) "
            "VALUES (%u, %u, %u, %u, %u)",
            guidLow, spec, uint32(itr->first),
            itr->second.GetAction(), uint32(itr->second.GetType()));

        ++buttonCount;
    }

    CharacterDatabase.CommitTransaction();

    PSendSysMessage("Saved spec %u: %u talents, %u action buttons.",
                    spec, uint32(talents.size()), buttonCount);
    return true;
}

// ############################################################################
// .spec show <n>
// ############################################################################
bool ChatHandler::HandleSpecShowCommand(char* args)
{
    Player* player = m_session ? m_session->GetPlayer() : nullptr;
    if (!player)
    {
        SendSysMessage("This command must be run in-game.");
        SetSentErrorMessage(true);
        return false;
    }

    uint32 spec = 0;
    if (!ExtractUInt32(&args, spec))
    {
        SendSysMessage("Syntax: .spec show <n>");
        SetSentErrorMessage(true);
        return false;
    }

    EnsureSpecTables();

    uint32 const guidLow = player->GetGUIDLow();

    std::vector<StoredTalent> talents;
    LoadStoredTalents(guidLow, spec, talents);

    std::vector<StoredButton> buttons;
    LoadStoredButtons(guidLow, spec, buttons);

    PSendSysMessage("Spec %u: %u talents, %u buttons stored.",
                    spec, uint32(talents.size()), uint32(buttons.size()));

    for (size_t i = 0; i < talents.size(); ++i)
    {
        TalentEntry const* talentInfo = sTalentStore.LookupEntry(talents[i].talentId);
        uint32 spellId = talentInfo ? talentInfo->RankID[talents[i].rank] : 0;

        PSendSysMessage("  talent %u rank %u -> spell %u",
                        talents[i].talentId, talents[i].rank + 1, spellId);
    }

    return true;
}

// ############################################################################
// .spec bars <n>
// ############################################################################
bool ChatHandler::HandleSpecBarsCommand(char* args)
{
    Player* player = m_session ? m_session->GetPlayer() : nullptr;
    if (!player)
    {
        SendSysMessage("This command must be run in-game.");
        SetSentErrorMessage(true);
        return false;
    }

    MasterPlayer* master = m_session->GetMasterPlayer();
    if (!master)
    {
        SendSysMessage("No MasterPlayer on this session.");
        SetSentErrorMessage(true);
        return false;
    }

    uint32 spec = 0;
    if (!ExtractUInt32(&args, spec))
    {
        SendSysMessage("Syntax: .spec bars <n>");
        SetSentErrorMessage(true);
        return false;
    }

    EnsureSpecTables();

    std::vector<StoredButton> buttons;
    LoadStoredButtons(player->GetGUIDLow(), spec, buttons);

    if (buttons.empty())
    {
        PSendSysMessage("No stored buttons for spec %u.", spec);
        SetSentErrorMessage(true);
        return false;
    }

    uint32 placed = RestoreButtons(master, buttons);
    PSendSysMessage("Restored %u of %u buttons for spec %u.",
                    placed, uint32(buttons.size()), spec);
    return true;
}

// ############################################################################
// .spec load <n>
// ############################################################################
bool ChatHandler::HandleSpecLoadCommand(char* args)
{
    Player* player = m_session ? m_session->GetPlayer() : nullptr;
    if (!player)
    {
        SendSysMessage("This command must be run in-game.");
        SetSentErrorMessage(true);
        return false;
    }

    MasterPlayer* master = m_session->GetMasterPlayer();
    if (!master)
    {
        SendSysMessage("No MasterPlayer on this session.");
        SetSentErrorMessage(true);
        return false;
    }

    uint32 spec = 0;
    if (!ExtractUInt32(&args, spec))
    {
        SendSysMessage("Syntax: .spec load <n>");
        SetSentErrorMessage(true);
        return false;
    }

    if (player->IsInCombat())
    {
        SendSysMessage("Not while in combat.");
        SetSentErrorMessage(true);
        return false;
    }

    EnsureSpecTables();

    uint32 const guidLow = player->GetGUIDLow();

    std::vector<StoredTalent> talents;
    LoadStoredTalents(guidLow, spec, talents);

    std::vector<StoredButton> buttons;
    LoadStoredButtons(guidLow, spec, buttons);

    if (talents.empty() && buttons.empty())
    {
        PSendSysMessage("Nothing stored for spec %u. Use .spec save %u first.", spec, spec);
        SetSentErrorMessage(true);
        return false;
    }

    // Free reset: noCost = true leaves m_resetTalentsMultiplier and
    // m_resetTalentsTime alone, so the respec economy is untouched.
    player->ResetTalents(true);

    uint32 failed = 0;
    uint32 applied = ReplayTalents(player, talents, failed);

    uint32 placed = RestoreButtons(master, buttons);

    PSendSysMessage("Spec %u loaded: %u/%u talents applied, %u/%u buttons placed.",
                    spec, applied, uint32(talents.size()), placed, uint32(buttons.size()));

    if (failed)
        PSendSysMessage("|cffff8080%u talents could not be applied|r - stale build, or the "
                        "stored points exceed this character's level.", failed);

    SendSysMessage("The client is about to clear buttons for the spells it just unlearned, "
                   "and those clears will delete restored buttons. Run '.spec bars <n>' now.");
    return true;
}
