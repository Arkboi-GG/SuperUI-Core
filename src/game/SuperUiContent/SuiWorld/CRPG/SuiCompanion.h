/*
 * SuperUI companions (owner decision 2026-09-02).
 *
 * A player's OWN other characters, summoned into the world as AiBot party
 * members for the length of the owner's session. The character is logged in
 * on a socket-less bot session that KEEPS ITS REAL ACCOUNT ID — only the
 * World session-map key is synthetic (WorldSession::GetSessionKey) so the
 * owner's live session and the companion can coexist. SaveToDB therefore
 * never re-stamps `characters.account`, which is the whole difference from
 * the Tesfff theft path: nothing has to be "swapped back", dismissal is a
 * plain logout.
 *
 * Authority law: a companion counts as a bot ONLY for its owner. To every
 * other human it is a real player — no possession, no orders, no bag/quest
 * access, no faction control. The same closure applies to an unattended own
 * character: real sessions other than the actor's own are never commandable.
 *
 * Lifetime: summon → bot session logs the character in (AiBotAI restart path,
 * the one owner-verified exception to the real-account wall) → first AI tick
 * joins the owner's group and teleports beside the owner's driven body →
 * dismissed explicitly, or when the owner's session logs its player out.
 * Never written to `characters.playerbot`; never opens the brain bridge.
 */

#ifndef MANGOS_SUI_COMPANION_H
#define MANGOS_SUI_COMPANION_H

#include "Common.h"
#include "ObjectGuid.h"

class Player;
class WorldSession;
class AiBotAI;

namespace SuiCompanion
{
    // CMSG_SUI_COMPANION actions
    enum Action : uint8
    {
        ACTION_SUMMON  = 1,
        ACTION_DISMISS = 2,
        ACTION_LIST    = 3,
    };

    // SMSG_SUI_COMPANION kind 1 result codes
    enum Result : uint8
    {
        RESULT_OK               = 0,
        RESULT_DENIED           = 1,   // not a character on the requester's account / unknown
        RESULT_ALREADY_IN_WORLD = 2,   // the character is online (you, or already summoned)
        RESULT_OWNER_STATE      = 3,   // requester not in world / dead / instance / taxi / transport / teleporting / a bot
        RESULT_LIMIT            = 4,   // MAX_COMPANIONS reached
        RESULT_NOT_A_COMPANION  = 5,   // dismiss target is not one of the requester's companions
        RESULT_FAILED           = 6,   // the bot manager refused to load it
        RESULT_PARTY_FULL       = 7,   // a full 5-man party: convert to a raid on purpose first
    };

    // SMSG_SUI_COMPANION kind 2 row states
    enum RowState : uint8
    {
        ROW_OFFLINE     = 0,   // summonable
        ROW_COMPANION   = 1,   // in the world as the requester's companion
        ROW_LOADING     = 2,   // summon in flight
        ROW_SELF        = 3,   // the character the requester is playing
        ROW_UNAVAILABLE = 4,   // online some other way (relogin race) — not summonable
    };

    static uint8 const MAX_COMPANIONS = 9;

    // ── Wire ──────────────────────────────────────────────────────────────
    void HandleCompanion(WorldSession* session, uint8 action, ObjectGuid guid);
    /// Push the account's character list (kind 2) to a SUI-capable session.
    void SendList(WorldSession* owner);

    // ── Shared by the wire and the GM command ─────────────────────────────
    Result Summon(WorldSession* owner, ObjectGuid guid);
    Result Dismiss(WorldSession* owner, ObjectGuid guid);

    // ── Identity ──────────────────────────────────────────────────────────
    bool IsCompanion(Player const* player);
    uint32 OwnerAccountOf(Player const* player);     // 0 when not a companion
    /// The owner's online character (the one that summoned it), or nullptr.
    Player* OwnerOf(Player const* companion);
    bool IsOwnedBy(Player const* actor, Player const* member);

    // ── THE authority predicate ───────────────────────────────────────────
    /// May `actor` treat `member` as a commandable bot? Self: yes. Another
    /// real session's character (attended or not): never. A companion: its
    /// owner only. A fabricated fleet bot: yes (group/faction rules apply on
    /// top, as before). Every possession / order / facts / item / quest site
    /// funnels through this so the rule cannot drift between copies.
    bool MayCommand(Player const* actor, Player const* member);

    // ── Hooks (core seams) ────────────────────────────────────────────────
    void OnCompanionInWorld(Player* companion);      // PlayerBotMgr::OnPlayerInWorld
    void TickArrival(AiBotAI* ai);                   // AiBotAI::UpdateAI while m_suiCompanionArrival
    void OnSessionLogout(WorldSession* session);     // WorldSession::LogoutPlayer (either side)
    /// Arrival visual: Tervosh's "teleport in" effect (script spell 7141), a
    /// vanilla self-cast with no gameplay effect that every client renders.
    static uint32 const ARRIVAL_VISUAL_SPELL = 7141;
    /// Ghost run speed used to size the self-run wait after a death nobody rezzes.
    static float const GHOST_RUN_SPEED = 7.0f;

    /// Tell the owner something about a companion (chat line), if they are online.
    void NotifyOwner(Player const* companion, char const* fmt, ...);

    /// Owner decision 2026-09-02: out of the owner's party = dismissed. A kicked
    /// companion, the owner leaving/being kicked, or a disband all log the
    /// companion(s) out with a save. Hold (RTS strip) is how you park one.
    void OnPlayerRemovedFromGroup(Player* removed);  // SuiPossess::OnPlayerRemovedFromGroup
}

#endif
