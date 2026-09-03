/*
 * SuperUI companions — see SuiCompanion.h for the design.
 *
 * Threading: the registry below is touched only on the world thread (wire
 * handlers are PACKET_PROCESS_WORLD; login/logout edges run there too). The
 * arrival step runs on the companion's MAP thread and reads nothing from the
 * registry — the owner guid rides on the AI itself (m_suiCompanionOwner),
 * which is set before the first map tick of the new player.
 */

#include "SuiCompanion.h"
#include "SuiPossess.h"
#include "AiBotAIMain.h"
#include "PlayerBotMgr.h"
#include "Player.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "World.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "Opcodes.h"
#include "Log.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Util.h"
#include "Server/Packets/SuiControl.h"
#include "PathFinder.h"

#include <unordered_map>
#include <vector>
#include <string>
#include <cstdarg>
#include <cstdio>

namespace SuiCompanion
{
namespace
{
    struct Record
    {
        uint32 ownerAccount = 0;
        ObjectGuid ownerGuid;      // the owner's character at summon time
        bool inWorld = false;      // AddToWorld happened; the arrival tick follows
    };

    // companion low guid -> record. World thread only.
    std::unordered_map<uint32, Record> s_companions;

    PlayerBotEntry* EntryOf(Player const* player)
    {
        if (!player)
            return nullptr;
        WorldSession* session = player->GetSession();
        return session ? session->GetBot() : nullptr;
    }

    uint32 CountFor(uint32 account)
    {
        uint32 n = 0;
        for (auto const& kv : s_companions)
            if (kv.second.ownerAccount == account)
                ++n;
        return n;
    }

    void SendResult(WorldSession* session, uint8 action, ObjectGuid guid, Result result)
    {
        if (!session || !session->IsSuiCapable())
            return;
        WorldPacket data(SMSG_SUI_COMPANION, 1 + 1 + 8 + 1);
        data << uint8(1);
        data << uint8(action);
        data << uint64(guid.GetRawValue());
        data << uint8(result);
        session->SendPacket(&data);
    }

    bool OwnerMaySummon(WorldSession* owner)
    {
        Player* player = owner->GetPlayer();
        if (!player || !player->IsInWorld() || owner->GetBot())
            return false;
        if (player->IsDead() || player->IsTaxiFlying() || player->GetTransport() ||
            player->IsBeingTeleported())
            return false;
        // Outdoors only: a bot session cannot be walked through an instance
        // portal on arrival, and instance binding of an alt is not this feature.
        MapEntry const* map = sMapStorage.LookupEntry<MapEntry>(player->GetMapId());
        if (!map || map->Instanceable())
            return false;
        return true;
    }

    // The body the owner is actually playing: the bot they possess, else themselves.
    Player* OwnerAnchor(Player* owner)
    {
        if (WorldSession* session = owner->GetSession())
            if (Player* driven = SuiPossess::GetControlledBot(session))
                if (driven->IsInWorld())
                    return driven;
        return owner;
    }
}

// ── Identity ─────────────────────────────────────────────────────────────

bool IsCompanion(Player const* player)
{
    PlayerBotEntry* entry = EntryOf(player);
    return entry && entry->ownerAccountId != 0;
}

uint32 OwnerAccountOf(Player const* player)
{
    PlayerBotEntry* entry = EntryOf(player);
    return entry ? entry->ownerAccountId : 0;
}

Player* OwnerOf(Player const* companion)
{
    PlayerBotEntry* entry = EntryOf(companion);
    if (!entry || !entry->ownerAccountId)
        return nullptr;
    Player* owner = sObjectMgr.GetPlayer(entry->ownerGuid);
    if (!owner || !owner->GetSession() || owner->GetSession()->GetBot())
        return nullptr;
    if (owner->GetSession()->GetAccountId() != entry->ownerAccountId)
        return nullptr;
    return owner;
}

bool IsOwnedBy(Player const* actor, Player const* member)
{
    PlayerBotEntry* entry = EntryOf(member);
    if (!entry || !entry->ownerAccountId || !actor || !actor->GetSession())
        return false;
    WorldSession* as = actor->GetSession();
    return !as->GetBot() && as->GetAccountId() == entry->ownerAccountId;
}

bool MayCommand(Player const* actor, Player const* member)
{
    if (!actor || !member)
        return false;
    if (actor == member)
        return true;
    WorldSession* ms = member->GetSession();
    if (!ms)
        return false;
    PlayerBotEntry* entry = ms->GetBot();
    if (!entry)
        return false;                       // another human's character: never
    if (entry->ownerAccountId != 0)
        return IsOwnedBy(actor, member);    // companion: its owner only
    return true;                            // shared fleet bot
}

// ── Summon / dismiss ─────────────────────────────────────────────────────

Result Summon(WorldSession* owner, ObjectGuid guid)
{
    if (!OwnerMaySummon(owner))
        return RESULT_OWNER_STATE;
    Player* player = owner->GetPlayer();
    if (guid.IsEmpty() || !guid.IsPlayer())
        return RESULT_DENIED;
    uint32 const low = guid.GetCounter();

    // THE rule: only characters of the requester's own account, verified from
    // the character cache — never from anything the client claims.
    uint32 const charAccount = sObjectMgr.GetPlayerAccountIdByGUID(guid);
    if (!charAccount || charAccount != owner->GetAccountId())
        return RESULT_DENIED;

    if (guid == player->GetObjectGuid() || sObjectAccessor.FindPlayer(guid) ||
        HashMapHolder<Player>::Find(guid))
        return RESULT_ALREADY_IN_WORLD;
    if (s_companions.count(low))
        return RESULT_ALREADY_IN_WORLD;     // summon already in flight
    if (CountFor(owner->GetAccountId()) >= MAX_COMPANIONS)
        return RESULT_LIMIT;
    // Owner 2026-09-02: never silently turn a party into a raid. The sixth body
    // is refused until the player converts on purpose.
    if (Group const* group = player->GetGroup())
        if (!group->isRaidGroup() && group->IsFull())
            return RESULT_PARTY_FULL;

    PlayerCacheData const* cache = sObjectMgr.GetPlayerDataByGUID(low);
    if (!cache)
        return RESULT_DENIED;

    AiBotAI* ai = new AiBotAI(uint8(cache->uiRace), uint8(cache->uiClass), cache->uiLevel,
        cache->uiMapId, 0, cache->fPosX, cache->fPosY, cache->fPosZ, cache->fOrientation);
    ai->SetSpawnName(cache->sName);
    ai->m_suiCompanionOwner = player->GetObjectGuid();

    // AddCompanion owns `ai` from here on, success or failure.
    if (!sPlayerBotMgr.AddCompanion(low, owner->GetAccountId(), player->GetObjectGuid(), ai))
        return RESULT_FAILED;

    Record rec;
    rec.ownerAccount = owner->GetAccountId();
    rec.ownerGuid = player->GetObjectGuid();
    s_companions[low] = rec;

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI-COMPANION] %s (account %u) summons %s (guid %u)",
        player->GetName(), owner->GetAccountId(), cache->sName.c_str(), low);
    return RESULT_OK;
}

Result Dismiss(WorldSession* owner, ObjectGuid guid)
{
    if (!owner->GetPlayer() || owner->GetBot())
        return RESULT_OWNER_STATE;
    if (guid.IsEmpty() || !guid.IsPlayer())
        return RESULT_NOT_A_COMPANION;
    auto it = s_companions.find(guid.GetCounter());
    if (it == s_companions.end() || it->second.ownerAccount != owner->GetAccountId())
        return RESULT_NOT_A_COMPANION;
    // Flags the bot entry OFFLINE: its session logs the character out (saving)
    // on the next world update, which fires OnSessionLogout below.
    sPlayerBotMgr.DeleteBot(guid.GetCounter());
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI-COMPANION] %s dismisses guid %u",
        owner->GetPlayer()->GetName(), guid.GetCounter());
    return RESULT_OK;
}

// ── Wire ─────────────────────────────────────────────────────────────────

void SendList(WorldSession* owner)
{
    if (!owner || !owner->IsSuiCapable())
        return;
    Player* self = owner->GetPlayer();
    uint32 const account = owner->GetAccountId();

    WorldPacket data(SMSG_SUI_COMPANION, 2 + 10 * 24);
    data << uint8(2);
    size_t const countPos = data.wpos();
    data << uint8(0);
    uint8 count = 0;

    auto result = CharacterDatabase.PQuery(
        "SELECT `guid`, `name`, `race`, `class`, `gender`, `level` FROM `characters` "
        "WHERE `account` = '%u' ORDER BY `guid`", account);
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 const low = fields[0].GetUInt32();
            ObjectGuid const guid(HIGHGUID_PLAYER, low);
            uint8 state = ROW_OFFLINE;
            if (self && guid == self->GetObjectGuid())
                state = ROW_SELF;
            else if (auto it = s_companions.find(low); it != s_companions.end())
                state = it->second.inWorld ? ROW_COMPANION : ROW_LOADING;
            else if (sObjectAccessor.FindPlayer(guid) || HashMapHolder<Player>::Find(guid))
                state = ROW_UNAVAILABLE;

            data << uint64(guid.GetRawValue());
            data << uint8(fields[2].GetUInt8());
            data << uint8(fields[3].GetUInt8());
            data << uint8(fields[4].GetUInt8());
            data << uint8(fields[5].GetUInt8());
            data << uint8(state);
            data << fields[1].GetCppString();   // NUL-terminated on the wire
            if (++count == 255)
                break;
        }
        while (result->NextRow());
    }
    data.put<uint8>(countPos, count);
    owner->SendPacket(&data);
}

void HandleCompanion(WorldSession* session, uint8 action, ObjectGuid guid)
{
    session->SetSuiCapable(true);
    switch (action)
    {
        case ACTION_SUMMON:
            SendResult(session, action, guid, Summon(session, guid));
            SendList(session);
            break;
        case ACTION_DISMISS:
            SendResult(session, action, guid, Dismiss(session, guid));
            SendList(session);
            break;
        case ACTION_LIST:
            SendList(session);
            break;
        default:
            break;
    }
}

// ── Hooks ────────────────────────────────────────────────────────────────

void OnCompanionInWorld(Player* companion)
{
    PlayerBotEntry* entry = EntryOf(companion);
    if (!entry || !entry->ownerAccountId)
        return;
    auto it = s_companions.find(companion->GetGUIDLow());
    if (it != s_companions.end())
        it->second.inWorld = true;
    if (AiBotAI* ai = dynamic_cast<AiBotAI*>(companion->AI()))
        ai->m_suiCompanionArrival = true;
    if (WorldSession* owner = sWorld.FindSession(entry->ownerAccountId))
        SendList(owner);
}

void TickArrival(AiBotAI* ai)
{
    // The flag stays up until the group work below is done: leaving a stale
    // saved group fires OnPlayerRemovedFromGroup, which must not read as a kick.
    struct ArrivalDone
    {
        AiBotAI* ai;
        ~ArrivalDone() { ai->m_suiCompanionArrival = false; }
    } arrivalDone{ai};
    Player* me = ai->me;
    if (!me || !me->IsInWorld())
        return;

    Player* owner = sObjectMgr.GetPlayer(ai->m_suiCompanionOwner);
    if (!owner || !owner->IsInWorld() || !owner->GetSession() || owner->GetSession()->GetBot())
    {
        // The owner left between summon and arrival: nothing to follow, go home.
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI-COMPANION] %s arrived without its owner, dismissing",
            me->GetName());
        sPlayerBotMgr.DeleteBot(me->GetGUIDLow());
        return;
    }

    // A saved group membership from the character's last real session is not
    // the owner's party; leave it before joining the right one.
    Group* group = owner->GetGroup();
    if (me->GetGroup() && me->GetGroup() != group)
        me->RemoveFromGroup();
    group = owner->GetGroup();
    if (!group)
    {
        // Mirror BridgeHandleFormGroup: Create does not publish to the registry.
        group = new Group;
        if (!group->Create(owner->GetObjectGuid(), owner->GetName()))
        {
            delete group;
            group = nullptr;
        }
        else
            sObjectMgr.AddGroup(group);
    }
    if (group && me->GetGroup() != group)
    {
        // Summon already refused a full party; a party that filled up in the
        // meantime keeps the companion ungrouped (it holds) rather than being
        // converted to a raid behind the owner's back.
        if (group->IsFull())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI-COMPANION] %s: %s's party is full, standing by",
                me->GetName(), owner->GetName());
            NotifyOwner(me, "%s is here but your party is full.", me->GetName());
        }
        else if (!group->AddMember(me->GetObjectGuid(), me->GetName()))
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[SUI-COMPANION] %s could not join %s's group",
                me->GetName(), owner->GetName());
    }

    // Arrive in formation behind whatever the owner is playing right now: slot 0
    // directly behind, later slots ring outward at 2.5 yd per ring, the same
    // fan the move order uses, so several summons never stack. The slot is this
    // companion's index among the owner's other companions already grouped.
    Player* anchor = OwnerAnchor(owner);
    uint32 slot = 0;
    if (Group* g = owner->GetGroup())
        for (GroupReference* itr = g->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (member && member != me && member->IsInWorld() && IsOwnedBy(owner, member))
                ++slot;
        }
    bool const sameMap = me->GetMapId() == anchor->GetMapId() &&
        me->GetInstanceId() == anchor->GetInstanceId();
    float x = anchor->GetPositionX(), y = anchor->GetPositionY(), z = anchor->GetPositionZ();
    if (sameMap)
    {
        // Owner 2026-09-02: the slot must be on the navmesh and reachable from
        // the anchor (a wall, a ledge, a doorway). Walk the ring from the
        // assigned slot outward; a slot the mesh cannot path to, or that the
        // path only reaches partially, is skipped. Nothing reachable = the
        // anchor's own feet, which are at least where the owner is standing.
        bool placed = false;
        for (uint32 attempt = 0; attempt < 18 && !placed; ++attempt)
        {
            uint32 const s = slot + attempt;
            uint32 const ring = s / 6 + 1;
            uint32 const pos = s % 6;
            float const angle = anchor->GetOrientation() + M_PI_F +
                float(pos) * (M_PI_F / 3.0f) + float(ring - 1) * (M_PI_F / 6.0f);
            float const radius = 2.5f * float(ring);
            float cx, cy, cz;
            anchor->GetNearPoint(anchor, cx, cy, cz, 0.0f, radius, angle);
            if (!anchor->IsWithinLOS(cx, cy, cz + 1.0f))
                continue;
            PathInfo probe(anchor);
            probe.calculate(cx, cy, cz);
            PathType const type = probe.getPathType();
            if (type & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT | PATHFIND_NOT_USING_PATH))
                continue;
            PointsArray const& pts = probe.getPath();
            if (pts.empty())
                continue;
            float const ex = pts.back().x, ey = pts.back().y, ez = pts.back().z;
            float const dx = ex - cx, dy = ey - cy;
            if (dx * dx + dy * dy > 1.5f * 1.5f)
                continue;               // the mesh moved the endpoint: slot is not really there
            x = ex; y = ey; z = ez;     // the mesh-snapped point, not the raw ring point
            placed = true;
            if (attempt)
                sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI-COMPANION] %s: slot %u unreachable, using slot %u",
                    me->GetName(), slot, s);
        }
        if (!placed)
            sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI-COMPANION] %s: no reachable slot around %s, arriving at its feet",
                me->GetName(), anchor->GetName());
        me->NearTeleportTo(x, y, z, anchor->GetOrientation());
        me->CastSpell(me, ARRIVAL_VISUAL_SPELL, true);
    }
    else
        // Cross-map: no mesh to probe from here; land on the owner's feet and let
        // formation follow sort out spacing after the transfer.
        me->TeleportTo(anchor->GetMapId(), x, y, z, anchor->GetOrientation());

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI-COMPANION] %s arrived beside %s (slot %u)",
        me->GetName(), anchor->GetName(), slot);
    NotifyOwner(me, "%s arrives.", me->GetName());
    SendList(owner->GetSession());
}

void NotifyOwner(Player const* companion, char const* fmt, ...)
{
    Player* owner = OwnerOf(companion);
    if (!owner || !owner->GetSession())
        return;
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    ChatHandler(owner->GetSession()).SendSysMessage(text);
}

void OnPlayerRemovedFromGroup(Player* removed)
{
    if (!removed || !removed->GetSession())
        return;
    if (PlayerBotEntry* entry = removed->GetSession()->GetBot())
    {
        if (!entry->ownerAccountId || entry->state == PB_STATE_OFFLINE)
            return;
        AiBotAI* ai = dynamic_cast<AiBotAI*>(removed->AI());
        if (ai && ai->m_suiCompanionArrival)
            return;                     // arrival housekeeping, not a kick
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI-COMPANION] %s left its owner's party: dismissed",
            removed->GetName());
        if (WorldSession* owner = sWorld.FindSession(entry->ownerAccountId))
            if (owner->GetPlayer())
                ChatHandler(owner).PSendSysMessage("%s left your party and was dismissed.",
                    removed->GetName());
        sPlayerBotMgr.DeleteBot(removed->GetGUIDLow());
        return;
    }
    // A real player parted from the group: every companion of that account that
    // was in it is now separated from its owner.
    uint32 const account = removed->GetSession()->GetAccountId();
    std::vector<uint32> mine;
    for (auto const& kv : s_companions)
        if (kv.second.ownerAccount == account)
            mine.push_back(kv.first);
    for (uint32 low : mine)
    {
        Player* companion = sObjectMgr.GetPlayer(ObjectGuid(HIGHGUID_PLAYER, low));
        if (!companion || !companion->GetGroup())
            continue;                   // not grouped: it was never in this party
        AiBotAI* ai = dynamic_cast<AiBotAI*>(companion->AI());
        if (ai && ai->m_suiCompanionArrival)
            continue;
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI-COMPANION] %s parted from %s's group: dismissed",
            companion->GetName(), removed->GetName());
        ChatHandler(removed->GetSession()).PSendSysMessage("%s was dismissed: you left the party.",
            companion->GetName());
        sPlayerBotMgr.DeleteBot(low);
    }
}

void OnSessionLogout(WorldSession* session)
{
    if (!session)
        return;
    if (PlayerBotEntry* entry = session->GetBot())
    {
        if (!entry->ownerAccountId)
            return;                         // a fleet bot: not ours
        // Companion leaving: drop out of the party first so no stale membership
        // is saved, forget the registry row, and refresh the owner's window.
        if (Player* me = session->GetPlayer())
            if (me->GetGroup())
                me->RemoveFromGroup();
        s_companions.erase(uint32(entry->playerGUID));
        sPlayerBotMgr.ForgetBot(uint32(entry->playerGUID));
        if (WorldSession* owner = sWorld.FindSession(entry->ownerAccountId))
            if (owner != session && owner->GetPlayer())
                SendList(owner);
        return;
    }

    // A real session logs its player out: every companion of the account goes too.
    uint32 const account = session->GetAccountId();
    std::vector<uint32> mine;
    for (auto const& kv : s_companions)
        if (kv.second.ownerAccount == account)
            mine.push_back(kv.first);
    for (uint32 low : mine)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI-COMPANION] owner account %u logging out, dismissing guid %u",
            account, low);
        sPlayerBotMgr.DeleteBot(low);
    }
}

} // namespace SuiCompanion

// ── Session shim ─────────────────────────────────────────────────────────

void WorldSession::HandleSuiCompanionOpcode(WorldPackets::SuiControl::Companion const& packet)
{
    if (!packet.exactSize)
        return;
    SuiCompanion::HandleCompanion(this, packet.action, packet.guid);
}

// ── GM command (stock-client testable path): .sui companion summon|dismiss <name> | list ──

bool ChatHandler::HandleSuiCompanionCommand(char* args)
{
    if (!m_session || !m_session->GetPlayer())
        return false;
    char* verb = strtok(args, " ");
    char* name = strtok(nullptr, " ");
    if (!verb)
    {
        SendSysMessage("Usage: .sui companion summon <name> | dismiss <name> | list");
        SetSentErrorMessage(true);
        return false;
    }
    std::string const v = verb;
    if (v == "list")
    {
        auto result = CharacterDatabase.PQuery(
            "SELECT `guid`, `name`, `level` FROM `characters` WHERE `account` = '%u' ORDER BY `guid`",
            m_session->GetAccountId());
        if (!result)
        {
            SendSysMessage("[SUI] no characters on this account");
            return true;
        }
        do
        {
            Field* f = result->Fetch();
            uint32 low = f[0].GetUInt32();
            ObjectGuid guid(HIGHGUID_PLAYER, low);
            char const* state = "offline";
            if (guid == m_session->GetPlayer()->GetObjectGuid())
                state = "you";
            else if (Player* p = sObjectAccessor.FindPlayer(guid))
                state = SuiCompanion::IsCompanion(p) ? "companion" : "online";
            PSendSysMessage("[SUI] %s (guid %u, level %u): %s", f[1].GetString(), low,
                f[2].GetUInt32(), state);
        }
        while (result->NextRow());
        return true;
    }
    if (!name)
    {
        SendSysMessage("Usage: .sui companion summon <name> | dismiss <name>");
        SetSentErrorMessage(true);
        return false;
    }
    ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(name);
    if (guid.IsEmpty())
    {
        PSendSysMessage("[SUI] no such character: %s", name);
        SetSentErrorMessage(true);
        return false;
    }
    if (v == "summon")
    {
        SuiCompanion::Result r = SuiCompanion::Summon(m_session, guid);
        PSendSysMessage("[SUI] summon %s: result %u", name, uint32(r));
        return true;
    }
    if (v == "dismiss")
    {
        SuiCompanion::Result r = SuiCompanion::Dismiss(m_session, guid);
        PSendSysMessage("[SUI] dismiss %s: result %u", name, uint32(r));
        return true;
    }
    SendSysMessage("Usage: .sui companion summon <name> | dismiss <name> | list");
    SetSentErrorMessage(true);
    return false;
}
