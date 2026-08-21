/* Same-faction AiBot discovery and direct-control authorization.
 *
 * BASELINE SuperUI (reclassified 2026-08-21 on the owner's word): every
 * deployment gets faction bot control groups out of the box, in normal
 * vanilla-mode worlds — commanding your own faction's AiBots is what SuperUI
 * IS. The RTS overlay only LAYERS on top of this service (hero row flags,
 * match mechanics); it is no longer the gate. Advertised to clients as
 * faction-control-groups-v1 in the SUI1 capability trailer. */

#include "SuiFactionControl.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "AiBotAIMain.h"
#include "DBCStores.h"
#include "Maps/Map.h"
#include "ObjectAccessor.h"
#include "Objects/Player.h"
#include "Server/WorldSession.h"
#include "SuiHero.h"
#include "SuiPossess.h"
#include "SuiRts.h"

namespace SuiFactionControl
{
namespace
{
    static uint8 const MAX_PAGE_ROWS = 200;
    static uint8 const ROW_STRIDE = 32;

    enum RowFlags : uint8
    {
        ROW_ALIVE       = 0x01,
        ROW_BUSY        = 0x02,
        ROW_ELIGIBLE    = 0x04,
        ROW_SAME_MAP    = 0x08,
        ROW_HERO        = 0x10,
        ROW_HERO_DEAD   = 0x20,
        ROW_INSTANCE    = 0x40,
    };

    struct Row
    {
        uint64 Guid = 0;
        uint32 GuidLow = 0;
        uint32 MapId = 0;
        uint32 ZoneId = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        uint8 Race = 0;
        uint8 Class = 0;
        uint8 Level = 0;
        uint8 Flags = 0;
    };

    bool IsAiBot(Player const* player)
    {
        if (!player || !player->GetSession() || !player->GetSession()->GetBot())
            return false;
        return dynamic_cast<AiBotAI*>(const_cast<Player*>(player)->AI()) != nullptr;
    }

    bool IsInstanceable(uint32 mapId)
    {
        MapEntry const* entry = sMapStorage.LookupEntry<MapEntry>(mapId);
        return !entry || entry->Instanceable();
    }
}

/// Baseline availability: always on. One seam kept so a future deployment
/// policy (a config kill-switch, a per-account gate) has exactly one place to
/// live; SuiRts::FactionControlEnabled() is deliberately NOT consulted — that
/// module now only decorates rosters with RTS-specific hero state.
bool Available()
{
    return true;
}

bool CanControl(Player const* actor, Player const* bot)
{
    return Available() && actor && bot && actor->IsInWorld() &&
        bot->IsInWorld() && actor->GetSession() && !actor->GetSession()->GetBot() &&
        IsAiBot(bot) && actor->GetTeam() == bot->GetTeam();
}

RelocateResult TryRelocate(Player* actor, Player* bot)
{
    if (!CanControl(actor, bot))
        return RELOCATE_NOT_ALLOWED;
    if (IsInstanceable(bot->GetMapId()) || bot->GetInstanceId() != 0)
        return RELOCATE_INSTANCE_DENIED;

    bool const restoreFreeView = SuiPossess::PrepareForRelocation(actor);
    if (!actor->TeleportTo(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
        bot->GetPositionZ(), bot->GetOrientation()))
    {
        SuiPossess::RestoreAfterFailedRelocation(actor, restoreFreeView);
        return RELOCATE_NOT_ALLOWED;
    }
    return RELOCATE_ACCEPTED;
}

void HandleRoster(WorldSession* session, uint8 flags, uint32 requestId,
    uint32 zoneId, uint32 afterGuidLow, uint8 requestedLimit)
{
    if (!session)
        return;
    session->SetSuiCapable(true);
    Player* actor = session->GetPlayer();

    // A zero correlation id cannot produce a reply the client is allowed to
    // accept. Drop it without scanning. Reserved flags on an otherwise valid
    // request receive a terminal empty page with the usable id echoed.
    if (requestId == 0)
        return;
    if (flags != 0)
    {
        WorldPacket data(SMSG_SUI_FORCE_ROSTER, 16);
        data << requestId << zoneId << uint32(0) << uint16(0) << uint8(0) << ROW_STRIDE;
        session->SendPacket(&data);
        return;
    }

    std::vector<Row> rows;
    if (Available() && actor && actor->IsInWorld() && !session->GetBot())
    {
        bool const actorReady = session->GetSuiControlledGuid().IsEmpty() &&
            actor->IsAlive() && !actor->IsTaxiFlying() && !actor->GetTransport() &&
            !actor->IsBeingTeleported() && actor->IsSelfMover();
        std::unordered_map<uint32, bool> heroDead;
        std::vector<SuiHero::Snapshot> heroes;
        SuiHero::SnapshotRows(heroes);
        for (SuiHero::Snapshot const& hero : heroes)
            if (hero.Team == (actor->GetTeam() == HORDE ? 1 : 0))
                heroDead[hero.GuidLow] = hero.Dead;

        HashMapHolder<Player>::ReadGuard guard(HashMapHolder<Player>::GetLock());
        for (auto const& entry : sObjectAccessor.GetPlayers())
        {
            Player* bot = entry.second;
            if (!CanControl(actor, bot) || (zoneId && bot->GetCachedZoneId() != zoneId))
                continue;

            Row row;
            row.Guid = bot->GetObjectGuid().GetRawValue();
            row.GuidLow = bot->GetGUIDLow();
            row.MapId = bot->GetMapId();
            row.ZoneId = bot->GetCachedZoneId();
            row.X = bot->GetPositionX();
            row.Y = bot->GetPositionY();
            row.Z = bot->GetPositionZ();
            row.Race = bot->GetRace();
            row.Class = bot->GetClass();
            row.Level = bot->GetLevel();

            bool busy = !bot->GetPossessorGuid().IsEmpty();
            bool sameMap = actor->GetMapId() == bot->GetMapId() &&
                actor->GetInstanceId() == bot->GetInstanceId();
            bool instance = IsInstanceable(bot->GetMapId()) || bot->GetInstanceId() != 0;
            bool basicState = bot->IsAlive() && !busy && !bot->IsTaxiFlying() &&
                !bot->GetTransport() && !bot->IsBeingTeleported();
            bool eligible = actorReady && basicState &&
                ((sameMap && actor->IsInVisibleList(bot)) || !instance);

            if (bot->IsAlive()) row.Flags |= ROW_ALIVE;
            if (busy) row.Flags |= ROW_BUSY;
            if (eligible) row.Flags |= ROW_ELIGIBLE;
            if (sameMap) row.Flags |= ROW_SAME_MAP;
            if (instance) row.Flags |= ROW_INSTANCE;
            auto hero = heroDead.find(row.GuidLow);
            if (hero != heroDead.end())
            {
                row.Flags |= ROW_HERO;
                if (hero->second)
                    row.Flags |= ROW_HERO_DEAD;
            }
            rows.push_back(row);
        }
    }

    std::sort(rows.begin(), rows.end(), [](Row const& left, Row const& right)
    {
        return left.GuidLow < right.GuidLow;
    });

    uint16 total = uint16(std::min<size_t>(0xFFFF, rows.size()));
    auto first = std::upper_bound(rows.begin(), rows.end(), afterGuidLow,
        [](uint32 guidLow, Row const& row) { return guidLow < row.GuidLow; });
    uint8 limit = requestedLimit == 0 ? MAX_PAGE_ROWS
        : std::min<uint8>(requestedLimit, MAX_PAGE_ROWS);
    size_t start = size_t(first - rows.begin());
    size_t end = std::min(rows.size(), start + size_t(limit));
    uint8 count = uint8(end - start);
    uint32 nextGuidLow = end < rows.size() && count ? rows[end - 1].GuidLow : 0;

    WorldPacket data(SMSG_SUI_FORCE_ROSTER, 16 + size_t(count) * ROW_STRIDE);
    data << requestId << zoneId << nextGuidLow << total << count << ROW_STRIDE;
    for (size_t i = start; i < end; ++i)
    {
        Row const& row = rows[i];
        data << row.Guid << row.MapId << row.ZoneId << row.X << row.Y << row.Z;
        data << row.Race << row.Class << row.Level << row.Flags;
    }
    session->SendPacket(&data);
}
}

void WorldSession::HandleSuiForceRosterOpcode(
    WorldPackets::SuiControl::ForceRoster const& packet)
{
    if (!packet.exactSize)
        return;

    SuiFactionControl::HandleRoster(this, packet.flags, packet.requestId,
        packet.zoneId, packet.afterGuidLow, packet.limit);
}
