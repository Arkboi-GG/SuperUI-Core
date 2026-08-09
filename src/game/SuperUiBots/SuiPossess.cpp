/*
 * SuperUI CRPG/RTS possession core. See SuiPossess.h for the state model.
 *
 * Ordering law (borrowed from Unit::ModPossess): Camera::SetView must precede
 * SetClientControl or the client ignores the control packets; SetMover must
 * precede the client's CMSG_SET_ACTIVE_MOVER confirmation or
 * HandleSetActiveMoverOpcode snaps the client mover back.
 */

#include "SuiPossess.h"

#include "AiBotAIMain.h"
#include "Chat.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Objects/Player.h"
#include "PlayerBotMgr.h"
#include "Server/WorldSession.h"
#include "World.h"

namespace SuiPossess
{

static void SendAck(WorldSession* session, ObjectGuid guid, AckResult result, Player* positionOf)
{
    // Custom SMSGs only ever go to clients that have spoken SUI (MSUIClient);
    // a stock 1.12 client driving a GM-command possession must not receive
    // opcodes beyond its table.
    if (!session->IsSuiCapable())
        return;
    WorldPacket data(SMSG_SUI_CONTROL_ACK, 8 + 1 + 4 * 4);
    data << uint64(guid.GetRawValue());
    data << uint8(result);
    if (positionOf)
        data << positionOf->GetPositionX() << positionOf->GetPositionY()
             << positionOf->GetPositionZ() << positionOf->GetOrientation();
    else
        data << 0.0f << 0.0f << 0.0f << 0.0f;
    session->SendPacket(&data);
}

Player* GetControlledBot(WorldSession const* session)
{
    ObjectGuid guid = session->GetSuiControlledGuid();
    return guid.IsEmpty() ? nullptr : sObjectMgr.GetPlayer(guid);
}

Player* GetPossessor(Unit const* bot)
{
    ObjectGuid possessorGuid = bot->GetPossessorGuid();
    if (possessorGuid.IsEmpty() || !possessorGuid.IsPlayer())
        return nullptr;
    Player* possessor = sObjectMgr.GetPlayer(possessorGuid);
    if (!possessor || !possessor->GetSession())
        return nullptr;
    // Distinguish SUI possession from spell (charm-based) possession.
    return possessor->GetSession()->GetSuiControlledGuid() == bot->GetObjectGuid()
        ? possessor : nullptr;
}

bool IsSuiPossessed(Unit const* unit)
{
    return GetPossessor(unit) != nullptr;
}

static AiBotAI* BotAiOf(Player* bot)
{
    return bot ? dynamic_cast<AiBotAI*>(bot->AI()) : nullptr;
}

/// Everything except the ACK — shared by the wire handler and the GM command.
static AckResult TryBegin(WorldSession* session, ObjectGuid targetGuid, Player** grantedBot)
{
    Player* possessor = session->GetPlayer();
    if (!possessor || !possessor->IsInWorld() || session->GetBot())
        return DENY_REQUESTER_STATE;
    if (!session->GetSuiControlledGuid().IsEmpty())
        return DENY_REQUESTER_STATE;    // release first
    if (possessor->IsDead() || possessor->IsTaxiFlying() || possessor->GetTransport() ||
        possessor->IsBeingTeleported() || !possessor->IsSelfMover())
        return DENY_REQUESTER_STATE;

    Player* bot = sObjectMgr.GetPlayer(targetGuid);
    if (!bot || !bot->IsInWorld())
        return DENY_NOT_FOUND;
    if (!bot->GetSession() || !bot->GetSession()->GetBot())
        return DENY_NOT_BOT;
    AiBotAI* ai = BotAiOf(bot);
    if (!ai)
        return DENY_NOT_BOT;
    Group* group = possessor->GetGroup();
    if (!group || group != bot->GetGroup())
        return DENY_NOT_IN_GROUP;
    if (!bot->GetPossessorGuid().IsEmpty())
        return DENY_BUSY;
    if (bot->IsDead() || bot->IsTaxiFlying() || bot->GetTransport() || bot->IsBeingTeleported())
        return DENY_TARGET_STATE;
    // Camera::SetView requires the target on the same map and visible to the client.
    if (bot->GetMapId() != possessor->GetMapId() ||
        bot->GetInstanceId() != possessor->GetInstanceId() ||
        !possessor->IsInVisibleList(bot))
        return DENY_NOT_FOUND;

    // ── Grant ──
    ai->SetPossessed(true);
    // A half-open loot window would strand the loot session (loot is
    // player-scoped); mirror ModPossess's force-release.
    if (ObjectGuid lootGuid = bot->GetLootGuid())
        bot->GetSession()->DoLootRelease(lootGuid);

    possessor->GetCamera().SetView(bot);
    bot->SetPossessorGuid(possessor->GetObjectGuid());
    session->SetSuiControlledGuid(bot->GetObjectGuid());
    possessor->SetMover(bot);
    possessor->SetClientControl(bot, 1);

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s now possesses bot %s",
        possessor->GetName(), bot->GetName());
    if (grantedBot)
        *grantedBot = bot;
    return ACK_OK;
}

/// Shared release path. Safe against a despawned bot or a tearing-down session.
static bool DoRelease(WorldSession* session, AckResult reason, bool serverInitiated)
{
    ObjectGuid botGuid = session->GetSuiControlledGuid();
    if (botGuid.IsEmpty())
        return false;
    session->SetSuiControlledGuid(ObjectGuid());

    Player* possessor = session->GetPlayer();
    if (Player* bot = sObjectMgr.GetPlayer(botGuid))
    {
        bot->SetPossessorGuid(ObjectGuid());
        if (AiBotAI* ai = BotAiOf(bot))
            ai->SetPossessed(false);
    }
    if (possessor)
    {
        possessor->GetCamera().ResetView();
        possessor->SetMover(nullptr);           // resolves to self
        possessor->SetClientControl(possessor, 1);
    }
    if (serverInitiated)
        // In-flight MSG_MOVE_* still carry bot coordinates; without the drain
        // window they would be attributed to the own character standing far
        // away (GetConfirmedMover fallback) and trip anticheat/teleport.
        session->RejectMovementPacketsFor(1000);

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s released bot %s (reason %u)",
        possessor ? possessor->GetName() : "<logging out>",
        botGuid.GetString().c_str(), uint32(reason));

    SendAck(session, possessor ? possessor->GetObjectGuid() : ObjectGuid(), reason, possessor);
    if (possessor && possessor->GetGroup())
        BroadcastRoster(possessor->GetGroup());
    return true;
}

void HandleRequest(WorldSession* session, ObjectGuid targetGuid)
{
    session->SetSuiCapable(true);
    Player* bot = nullptr;
    AckResult result = TryBegin(session, targetGuid, &bot);
    SendAck(session, targetGuid, result, bot);
    if (result == ACK_OK && bot)
    {
        // M3 follows the grant with proxied SMSG_INITIAL_SPELLS /
        // SMSG_ACTION_BUTTONS and the SMSG_SUI_SNAPSHOT.
        if (Group* group = session->GetPlayer()->GetGroup())
            BroadcastRoster(group);
    }
}

void HandleRelease(WorldSession* session, uint8 mode)
{
    session->SetSuiCapable(true);
    AckResult reason = mode == RELEASE_TO_FREECAM ? RELEASED_FREECAM : RELEASED;
    if (!DoRelease(session, reason, false))
    {
        // Nothing possessed: this is the freecam enter/leave path from the own
        // character (M5 attaches/detaches the unattended AI here). Ack so the
        // client state machine resolves either way.
        SendAck(session, session->GetPlayer() ? session->GetPlayer()->GetObjectGuid() : ObjectGuid(),
            reason, session->GetPlayer());
    }
}

void ForceRelease(WorldSession* session, AckResult reason)
{
    DoRelease(session, reason, true);
}

// ── Hooks ────────────────────────────────────────────────────────────────────

void OnPlayerRemovedFromGroup(Player* player)
{
    if (!player)
        return;
    if (Player* possessor = GetPossessor(player))
        ForceRelease(possessor->GetSession(), RELEASED_GROUP);
    else if (player->GetSession() && !player->GetSession()->GetSuiControlledGuid().IsEmpty())
        ForceRelease(player->GetSession(), RELEASED_GROUP);
}

void OnPlayerTeleport(Player* player)
{
    if (!player)
        return;
    if (Player* possessor = GetPossessor(player))
        ForceRelease(possessor->GetSession(), RELEASED_TELEPORT);
    else if (player->GetSession() && !player->GetSession()->GetSuiControlledGuid().IsEmpty())
        ForceRelease(player->GetSession(), RELEASED_TELEPORT);
}

void OnPlayerDeath(Player* player)
{
    // Only the possessed bot's death breaks possession; the possessor's own
    // character dying under AI is surfaced client-side, not force-released.
    if (Player* possessor = GetPossessor(player))
        ForceRelease(possessor->GetSession(), RELEASED_DEATH);
}

void OnLogout(WorldSession* session)
{
    // Session was possessing someone → clean release.
    DoRelease(session, RELEASED_LOGOUT, true);
    // Session's player IS a possessed bot (bot despawn path) → release its human.
    if (Player* player = session->GetPlayer())
        if (Player* possessor = GetPossessor(player))
            ForceRelease(possessor->GetSession(), RELEASED_LOGOUT);
}

// ── Roster ───────────────────────────────────────────────────────────────────

void SendRoster(Player* realPlayer)
{
    WorldSession* session = realPlayer->GetSession();
    if (!session || session->GetBot() || !session->IsSuiCapable())
        return;

    WorldPacket data(SMSG_SUI_CONTROL_ROSTER, 1 + 9 * MAX_RAID_SIZE);
    Group* group = realPlayer->GetGroup();
    if (!group)
    {
        data << uint8(0);
        session->SendPacket(&data);
        return;
    }

    size_t countPos = data.wpos();
    data << uint8(0);
    uint8 count = 0;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member || !member->IsInWorld())
            continue;
        uint8 flags = 0;
        if (member->GetSession() && member->GetSession()->GetBot() && BotAiOf(member))
            flags |= ROSTER_CONTROLLABLE;
        if (IsSuiPossessed(member))
            flags |= ROSTER_POSSESSED;
        data << uint64(member->GetObjectGuid().GetRawValue());
        data << flags;
        ++count;
    }
    data.put<uint8>(countPos, count);
    session->SendPacket(&data);
}

void BroadcastRoster(Group* group)
{
    if (!group)
        return;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (member && member->GetSession() && !member->GetSession()->GetBot())
            SendRoster(member);
    }
}

} // namespace SuiPossess

// ── Wire handlers ────────────────────────────────────────────────────────────

void WorldSession::HandleSuiControlRequestOpcode(WorldPackets::SuiControl::ControlRequest const& packet)
{
    SuiPossess::HandleRequest(this, packet.targetGuid);
}

void WorldSession::HandleSuiControlReleaseOpcode(WorldPackets::SuiControl::ControlRelease const& packet)
{
    SuiPossess::HandleRelease(this, packet.mode);
}

// ── GM commands (stock-client testable: .sui possess <name> / .sui release) ──

bool ChatHandler::HandleSuiPossessCommand(char* args)
{
    Player* requester = m_session ? m_session->GetPlayer() : nullptr;
    if (!requester)
        return false;

    ObjectGuid targetGuid;
    if (char* name = ExtractLiteralArg(&args))
    {
        std::string playerName = name;
        if (Player* target = sObjectMgr.GetPlayer(playerName.c_str()))
            targetGuid = target->GetObjectGuid();
    }
    else if (Player* selected = GetSelectedPlayer())
        targetGuid = selected->GetObjectGuid();

    if (targetGuid.IsEmpty())
    {
        SendSysMessage("[SUI] usage: .sui possess <botname> (or select the bot)");
        SetSentErrorMessage(true);
        return false;
    }

    Player* bot = nullptr;
    SuiPossess::AckResult result = SuiPossess::TryBegin(m_session, targetGuid, &bot);
    if (result == SuiPossess::ACK_OK)
        PSendSysMessage("[SUI] possessing %s — WASD drives the bot, .sui release to stop",
            bot ? bot->GetName() : "?");
    else
        PSendSysMessage("[SUI] possess denied (code %u)", uint32(result));
    return true;
}

bool ChatHandler::HandleSuiReleaseCommand(char* /*args*/)
{
    if (!m_session)
        return false;
    if (m_session->GetSuiControlledGuid().IsEmpty())
    {
        SendSysMessage("[SUI] not possessing anything");
        return true;
    }
    // Voluntary GM release still opens the drain window: a stock client has no
    // pending-state machine parking its movement stream first.
    SuiPossess::ForceRelease(m_session, SuiPossess::RELEASED);
    SendSysMessage("[SUI] released");
    return true;
}
