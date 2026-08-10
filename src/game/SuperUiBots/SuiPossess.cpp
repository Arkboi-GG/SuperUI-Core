/*
 * SuperUI CRPG/RTS possession core. See SuiPossess.h for the state model.
 *
 * Ordering law (borrowed from Unit::ModPossess): Camera::SetView must precede
 * SetClientControl or the client ignores the control packets; SetMover must
 * precede the client's CMSG_SET_ACTIVE_MOVER confirmation or
 * HandleSetActiveMoverOpcode snaps the client mover back.
 */

#include "SuiPossess.h"
#include <unordered_map>
#include <cmath>

#include "AiBotAIMain.h"
#include "Bag.h"
#include "Chat.h"
#include "Group.h"
#include "MasterPlayer.h"
#include "ObjectMgr.h"
#include "Objects/Player.h"
#include "PlayerBotMgr.h"
#include "Server/WorldSession.h"
#include "AiBotAIMain.h"
#include "World.h"

namespace SuiPossess
{

static void SendSnapshot(WorldSession* to, Player* bot);   // defined with the M4 block below

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

// ── Own-character autonomy (M5, design corrected 2026-08-10) ─────────────────
// While the human drives a bot or the free camera, their real character runs
// the SAME fleet AI as every SuperUI bot (AiBotAI): it enrolls with the C#
// brain through the normal HELLO/STATE bridge flow, follows the party boss
// (possessed-first pre-pass in FindPartyBoss, so no anchor plumbing), and
// assists in combat. In-party behaviour is the gate — group break force-
// releases and detaches — and the bridge SELL_ITEMS wall refuses live-client
// sessions, so it can never vendor. Never stomps a foreign AI (mind control);
// deletion is explicit — this AI is owned here, not by PlayerBotMgr.

static void AttachUnattendedAI(Player* owner, Player* /*anchor*/)
{
    if (!owner || !owner->IsInWorld())
        return;
    if (dynamic_cast<AiBotAI*>(owner->AI()))
        return;   // already enrolled (bot switch / freecam transition)
    if (owner->AI())
        return;   // charmed/controlled by something else — leave it alone
    AiBotAI::AttachToRealCharacter(owner);
}

static void DetachUnattendedAI(Player* owner)
{
    if (!owner)
        return;
    AiBotAI* ai = dynamic_cast<AiBotAI*>(owner->AI());
    if (!ai)
        return;
    owner->SetAI(nullptr);
    delete ai;
    owner->StopMoving();
    owner->GetMotionMaster()->Clear(false, true);
    owner->GetMotionMaster()->MoveIdle();
    owner->AttackStop();
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s back under manual control", owner->GetName());
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

    // The abandoned real character follows and assists whoever the human drives.
    AttachUnattendedAI(possessor, bot);

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s now possesses bot %s",
        possessor->GetName(), bot->GetName());
    if (grantedBot)
        *grantedBot = bot;
    return ACK_OK;
}

/// Shared release path. Safe against a despawned bot or a tearing-down session.
// ── Freecam eye ──────────────────────────────────────────────────────────────
// The free view must LOAD what it overflies. Visibility/grid streaming follows
// the player's Camera, so while in the free view the camera rides an invisible
// World Trigger summon (active object: keeps its grid ticking) that the client
// repositions with CMSG_SUI_CAM. Torn down on every path that leaves the view.
static std::unordered_map<uint64, uint64> s_freecamEyes;

static Creature* FreecamEyeOf(Player* player)
{
    auto it = s_freecamEyes.find(player->GetObjectGuid().GetRawValue());
    if (it == s_freecamEyes.end())
        return nullptr;
    return player->GetMap()->GetCreature(ObjectGuid(it->second));
}

static void EnsureFreecamEye(Player* player)
{
    if (!player || !player->IsInWorld())
        return;
    if (Creature* existing = FreecamEyeOf(player))
    {
        player->GetCamera().SetView(existing);
        return;
    }
    Creature* eye = player->SummonCreature(15384 /* World Trigger, invisible model */,
        player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), 0.0f,
        TEMPSUMMON_MANUAL_DESPAWN, 0, true /* active object */);
    if (!eye)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[SUI] %s: freecam eye summon failed",
            player->GetName());
        return;
    }
    eye->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);
    s_freecamEyes[player->GetObjectGuid().GetRawValue()] = eye->GetObjectGuid().GetRawValue();
    player->GetCamera().SetView(eye);
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI] %s: freecam eye up", player->GetName());
}

static void RemoveFreecamEye(Player* player)
{
    if (!player)
        return;
    auto it = s_freecamEyes.find(player->GetObjectGuid().GetRawValue());
    if (it == s_freecamEyes.end())
        return;
    if (player->IsInWorld())
        if (Creature* eye = player->GetMap()->GetCreature(ObjectGuid(it->second)))
        {
            player->GetCamera().ResetView();
            eye->AddObjectToRemoveList();
        }
    s_freecamEyes.erase(it);
}

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
        // Manual control resumes — except into the free camera, where the own
        // character stays autonomous. Its anchor remains the just-released bot
        // (still a valid group member); it only re-points on the next possess.
        if (reason != RELEASED_FREECAM)
        {
            DetachUnattendedAI(possessor);
            RemoveFreecamEye(possessor);
        }
        else
            EnsureFreecamEye(possessor);
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
    if (Player* requester = session->GetPlayer())
        RemoveFreecamEye(requester);   // possess overrides the free-camera view
    Player* bot = nullptr;
    AckResult result = TryBegin(session, targetGuid, &bot);
    SendAck(session, targetGuid, result, bot);
    if (result == ACK_OK && bot)
    {
        // Owner data follows the grant, deliberately routed through the BOT's
        // own (socket-less) session: the SendPacket mirror wraps each packet in
        // SMSG_SUI_PROXY with the right source guid. Mid-session re-sends of
        // both packets are proven safe in this fork (SpecCommands .testbars).
        bot->SendInitialSpells();
        if (MasterPlayer* master = bot->GetSession()->GetMasterPlayer())
            master->SendInitialActionButtons();
        SendSnapshot(session, bot);
        if (Group* group = session->GetPlayer()->GetGroup())
            BroadcastRoster(group);
    }
}

void HandleCam(WorldSession* session, float x, float y, float z)
{
    Player* player = session->GetPlayer();
    if (!player || !player->IsInWorld())
        return;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return;
    if (Creature* eye = FreecamEyeOf(player))
        eye->NearTeleportTo(x, y, z, 0.0f);
}

void HandleRelease(WorldSession* session, uint8 mode)
{
    session->SetSuiCapable(true);
    AckResult reason = mode == RELEASE_TO_FREECAM ? RELEASED_FREECAM : RELEASED;
    if (!DoRelease(session, reason, false))
    {
        // Nothing possessed: the freecam enter/leave path from the own character.
        if (Player* player = session->GetPlayer())
        {
            if (mode == RELEASE_TO_FREECAM)
            {
                Player* anchor = nullptr;
                if (Group* group = player->GetGroup())
                    anchor = sObjectMgr.GetPlayer(group->GetLeaderGuid());
                AttachUnattendedAI(player, anchor && anchor != player ? anchor : nullptr);
                EnsureFreecamEye(player);
            }
            else
            {
                DetachUnattendedAI(player);
                RemoveFreecamEye(player);
            }
        }
        // Ack so the client state machine resolves either way.
        SendAck(session, session->GetPlayer() ? session->GetPlayer()->GetObjectGuid() : ObjectGuid(),
            reason, session->GetPlayer());
    }
}

void ForceRelease(WorldSession* session, AckResult reason)
{
    DoRelease(session, reason, true);
}

void HandleOrder(WorldSession* session, uint8 orderType,
    std::vector<ObjectGuid> const& subjects, ObjectGuid targetGuid,
    float x, float y, float z)
{
    session->SetSuiCapable(true);
    Player* player = session->GetPlayer();
    if (!player || session->GetBot())
        return;
    Group* group = player->GetGroup();
    if (!group)
        return;

    auto orderBot = [&](Player* pMember)
    {
        if (!pMember || pMember->GetGroup() != group)
            return;
        // AI-attached is the real gate: fabricated bots always are; the human's
        // own character only while unattended (possession/freecam), which is
        // exactly when it must obey RTS orders alongside the bots. A manually
        // driven character has no AiBotAI, and the possessed bot stays excluded.
        AiBotAI* ai = dynamic_cast<AiBotAI*>(pMember->AI());
        if (!ai || ai->IsPossessed())
            return;

        // Reuse the bridge command paths verbatim — ordered behaviour is then
        // bit-identical to a brain-issued MOVE_TO / ATTACK_TARGET, including the
        // chunked pathfinding and the in-combat MOVE_TO deferral. The PlayerParty
        // escort loop yields to an active TASK_MOVE_TO, so orders are not
        // formation-snapped on the next tick.
        char json[192];
        switch (orderType)
        {
            case ORDER_MOVE:
                ai->SuiClearWaypoints();   // a plain move order replaces any chain
                snprintf(json, sizeof(json),
                    "{\"type\":\"MOVE_TO\",\"payload\":{\"mapId\":%u,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
                    pMember->GetMapId(), x, y, z);
                ai->BridgeProcessLine(json);
                break;
            case ORDER_ATTACK:
                if (targetGuid.IsCreature())
                {
                    snprintf(json, sizeof(json),
                        "{\"type\":\"ATTACK_TARGET\",\"payload\":{\"guid\":%u}}",
                        targetGuid.GetCounter());
                    ai->BridgeProcessLine(json);
                }
                break;
            case ORDER_MOVE_QUEUE:
                ai->SuiQueueWaypoint(x, y, z);
                break;
            case ORDER_STOP:
                ai->SuiClearWaypoints();
                ai->StopMoving();
                pMember->AttackStop();
                ai->m_currentTask.type = TASK_IDLE;
                pMember->GetMotionMaster()->MoveIdle();
                break;
            default:
                break;
        }
    };

    if (!subjects.empty())
        for (ObjectGuid guid : subjects)
            orderBot(sObjectMgr.GetPlayer(guid));
    else
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            orderBot(itr->getSource());
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
    if (Player* player = session->GetPlayer())
    {
        // Freecam logout: the unattended AI is owned here, not by a bot entry —
        // reclaim it before Player teardown.
        DetachUnattendedAI(player);
        RemoveFreecamEye(player);
        // Session's player IS a possessed bot (bot despawn path) → release its human.
        if (Player* possessor = GetPossessor(player))
            ForceRelease(possessor->GetSession(), RELEASED_LOGOUT);
    }
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

// ── Owner-data mirror (M3) + inventory/talent snapshot (M4) ──────────────────

namespace SuiPossess
{

static void AppendSnapshotItem(WorldPacket& data, uint8 bag, uint8 slot, Item* item)
{
    data << uint8(bag);
    data << uint8(slot);
    data << uint64(item->GetObjectGuid().GetRawValue());
    data << uint32(item->GetEntry());
    data << uint32(item->GetCount());
    uint8 bagSlots = 0;
    if (ItemPrototype const* proto = item->GetProto())
        if (proto->Class == ITEM_CLASS_CONTAINER)
            bagSlots = (uint8)((Bag*)item)->GetBagSize();
    data << bagSlots;
}

/// Read-only bags + talent points for the possessed bot, pushed once per grant.
/// bag 255 = character-held (equipment 0-18, bag slots 19-22, backpack 23-38,
/// keyring 81+ — one contiguous slot numbering); bag 19-22 = inside that
/// equipped bag. Bag rows precede their contents by construction.
static void SendSnapshot(WorldSession* to, Player* bot)
{
    if (!to->IsSuiCapable())
        return;
    WorldPacket data(SMSG_SUI_SNAPSHOT, 4096);
    data << uint64(bot->GetObjectGuid().GetRawValue());
    data << uint32(bot->GetUInt32Value(PLAYER_CHARACTER_POINTS1));
    data << uint32(bot->GetMoney());
    size_t countPos = data.wpos();
    data << uint16(0);
    uint16 count = 0;

    for (uint8 i = EQUIPMENT_SLOT_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            AppendSnapshotItem(data, 255, i, item);
            ++count;
        }
    for (uint8 i = KEYRING_SLOT_START; i < KEYRING_SLOT_END; ++i)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            AppendSnapshotItem(data, 255, i, item);
            ++count;
        }
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Item* bagItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
            if (ItemPrototype const* proto = bagItem->GetProto())
                if (proto->Class == ITEM_CLASS_CONTAINER)
                {
                    Bag* pBag = (Bag*)bagItem;
                    for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                        if (Item* item = pBag->GetItemByPos((uint8)j))
                        {
                            AppendSnapshotItem(data, bagSlot, (uint8)j, item);
                            ++count;
                        }
                }

    data.put<uint16>(countPos, count);
    to->SendPacket(&data);
}

void MirrorOwnerPacket(WorldSession* botSession, WorldPacket const* packet)
{
    // Whitelist first: this sits on every socket-less SendPacket, keep it cheap.
    // Mirroring everything would double-deliver broadcasts the possessor already
    // receives; these are the strictly owner-only spell/bar/cooldown packets.
    switch (packet->GetOpcode())
    {
        case SMSG_ACTION_BUTTONS:
        case SMSG_INITIAL_SPELLS:
        case SMSG_LEARNED_SPELL:
        case SMSG_SUPERCEDED_SPELL:
        case SMSG_REMOVED_SPELL:
        case SMSG_SPELL_COOLDOWN:
        case SMSG_COOLDOWN_EVENT:
        case SMSG_CLEAR_COOLDOWN:
        case SMSG_CAST_RESULT:
            break;
        default:
            return;
    }

    Player* bot = botSession->GetPlayer();
    if (!bot)
        return;
    Player* possessor = GetPossessor(bot);
    if (!possessor || !possessor->GetSession() || !possessor->GetSession()->IsSuiCapable())
        return;

    WorldPacket data(SMSG_SUI_PROXY, 8 + 2 + packet->size());
    data << uint64(bot->GetObjectGuid().GetRawValue());
    data << uint16(packet->GetOpcode());
    if (packet->size() > 0)
        data.append(packet->contents(), packet->size());
    possessor->GetSession()->SendPacket(&data);
}

} // namespace SuiPossess

// ── Wire handlers ────────────────────────────────────────────────────────────

Player* WorldSession::GetSuiActor()
{
    // The unit the session's gameplay input acts as: the possessed bot while
    // driving one, else the session's own player. Threaded through the
    // cast/target/melee handler family — bit-identical when not possessing.
    if (!m_suiControlledGuid.IsEmpty())
        if (Player* bot = SuiPossess::GetControlledBot(this))
            return bot;
    return _player;
}

void WorldSession::HandleSuiControlRequestOpcode(WorldPackets::SuiControl::ControlRequest const& packet)
{
    SuiPossess::HandleRequest(this, packet.targetGuid);
}

void WorldSession::HandleSuiControlReleaseOpcode(WorldPackets::SuiControl::ControlRelease const& packet)
{
    SuiPossess::HandleRelease(this, packet.mode);
}

void WorldSession::HandleSuiOrderOpcode(WorldPackets::SuiControl::Order const& packet)
{
    SuiPossess::HandleOrder(this, packet.orderType, packet.subjects,
        packet.targetGuid, packet.x, packet.y, packet.z);
}

void WorldSession::HandleSuiCamOpcode(WorldPackets::SuiControl::Cam const& packet)
{
    SuiPossess::HandleCam(this, packet.x, packet.y, packet.z);
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
