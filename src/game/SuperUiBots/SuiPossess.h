/*
 * SuperUI CRPG/RTS possession core.
 *
 * A real client session takes direct control of a party AiBot: the session's
 * mover is re-pointed at the bot (stock mover machinery — MSG_MOVE_* then
 * drive the bot), the bot's autonomous AI is suspended, and owner-only data is
 * proxied back to the possessor (M3). Deliberately NOT built on the charm
 * aura machinery: ModPossess is mind-control semantics (faction swap, threat
 * wipe) which is wrong for driving your own party bot mid-fight. Only the
 * ordering law is borrowed: Camera::SetView BEFORE SetMover BEFORE
 * SetClientControl (see Unit::ModPossess).
 *
 * State model (no global registry):
 *   - possessor session:  WorldSession::m_suiControlledGuid
 *   - bot:                Unit::m_possessorGuid (SetPossessorGuid) — also
 *                         denies a second possessor and routes
 *                         GetConfirmedMover/UpdateControl correctly
 *   - bot AI:             AiBotAI::m_possessed (suspends behaviour + bridge
 *                         commands; bridge STATE keeps flowing with
 *                         possessed:1 so the C# brain stands down)
 */

#ifndef MANGOS_SUI_POSSESS_H
#define MANGOS_SUI_POSSESS_H

#include "Common.h"
#include "ObjectGuid.h"

#include <vector>

class Player;
class Unit;
class WorldSession;
class WorldPacket;
class Group;

namespace SuiPossess
{
    // SMSG_SUI_CONTROL_ACK result codes. 0 = granted; 1..15 denials answer a
    // CMSG_SUI_CONTROL_REQUEST; 16+ are releases — solicited or forced — and
    // must be accepted by the client in ANY control state.
    enum AckResult : uint8
    {
        ACK_OK                  = 0,
        DENY_NOT_FOUND          = 1,   // target missing / not a player / not visible / other map
        DENY_NOT_BOT            = 2,   // target is a real player or has no AiBotAI
        DENY_NOT_IN_GROUP       = 3,   // requester and target not in the same group
        DENY_BUSY               = 4,   // already possessed by someone
        DENY_TARGET_STATE       = 5,   // dead / taxi / transport / teleporting
        DENY_REQUESTER_STATE    = 6,   // requester is a bot, not self-mover, dead, on taxi…

        RELEASED                = 16,  // voluntary, back to own character
        RELEASED_FREECAM        = 17,  // voluntary, own character stays autonomous
        RELEASED_DEATH          = 18,  // possessed bot died
        RELEASED_TELEPORT       = 19,  // either side got teleported / changed map
        RELEASED_GROUP          = 20,  // group membership between the pair broke
        RELEASED_LOGOUT         = 21,  // possessor logging out, or the bot despawned
    };

    enum ReleaseMode : uint8
    {
        RELEASE_TO_SELF     = 0,
        RELEASE_TO_FREECAM  = 1,
    };

    enum OrderType : uint8              // CMSG_SUI_ORDER (M6)
    {
        ORDER_MOVE   = 0,
        ORDER_ATTACK = 1,
        ORDER_STOP   = 2,
        ORDER_MOVE_QUEUE = 3,   // append a waypoint; arrival chains the next leg
    };

    // SMSG_SUI_CONTROL_ROSTER member flags
    enum RosterFlags : uint8
    {
        ROSTER_CONTROLLABLE = 0x01,    // AiBot in your group you may possess
        ROSTER_POSSESSED    = 0x02,    // currently driven by a real player
    };

    /// Attempt possession. Sends the ACK (grant or deny) itself.
    void HandleRequest(WorldSession* session, ObjectGuid targetGuid);

    /// Voluntary release (client asked). Safe to call when nothing is possessed.
    void HandleRelease(WorldSession* session, uint8 mode);

    /// RTS order from the free camera: move/attack/stop for the group's AiBots
    /// (explicit subject list, or empty = every controllable bot). Reuses the
    /// bridge command paths so ordered movement/attack behaves exactly like a
    /// brain-issued command.
    void HandleOrder(WorldSession* session, uint8 orderType,
        std::vector<ObjectGuid> const& subjects, ObjectGuid targetGuid,
        float x, float y, float z);

    /// Forced release with a reason code; no-op when the session possesses nothing.
    /// Server-initiated paths open the movement drain window (m_moveRejectTime) so
    /// in-flight bot-coordinate MSG_MOVE_* are not attributed to the distant own char.
    void ForceRelease(WorldSession* session, AckResult reason);

    // ── Hooks (called from core seams) ────────────────────────────────────────
    void OnPlayerRemovedFromGroup(Player* player);   // Group::RemoveMember / Disband
    void OnPlayerTeleport(Player* player);           // Player::TeleportTo (either side of the pair)
    void OnPlayerDeath(Player* player);              // Player::SetDeathState(JUST_DIED)
    void OnLogout(WorldSession* session);            // WorldSession::LogoutPlayer

    // ── Queries ───────────────────────────────────────────────────────────────
    /// The real player driving `bot` via SUI possession, or nullptr.
    Player* GetPossessor(Unit const* bot);
    /// The bot this session is driving, or nullptr.
    Player* GetControlledBot(WorldSession const* session);
    bool IsSuiPossessed(Unit const* unit);

    // ── Roster ────────────────────────────────────────────────────────────────
    /// Push SMSG_SUI_CONTROL_ROSTER to one real player (their current group view).
    void SendRoster(Player* realPlayer);
    /// Push the roster to every real-session member of a group.
    void BroadcastRoster(Group* group);

    // ── Owner-data mirror (M3) ────────────────────────────────────────────────
    /// Wrap whitelisted owner-only packets of a possessed bot's socket-less
    /// session into SMSG_SUI_PROXY toward the possessor. Called from
    /// WorldSession::SendPacket; no-op unless possessed and whitelisted.
    void MirrorOwnerPacket(WorldSession* botSession, WorldPacket const* packet);
}

#endif
