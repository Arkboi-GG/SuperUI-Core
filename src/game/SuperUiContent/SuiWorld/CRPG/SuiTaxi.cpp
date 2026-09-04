/*
 * SuperUI party flight — see SuiTaxi.h.
 */

#include "SuiTaxi.h"
#include "SuiCompanion.h"
#include "SuiPossess.h"
#include "SuiTacticalFreeze.h"
#include "AiBotAIMain.h"
#include "Player.h"
#include "Creature.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "ObjectMgr.h"
#include "Group.h"
#include "Opcodes.h"
#include "Log.h"
#include "MotionMaster.h"
#include "SpellAuraDefines.h"
#include "DBCStores.h"
#include "Server/Packets/SuiControl.h"

namespace SuiTaxi
{

namespace
{

struct Row
{
    ObjectGuid guid;
    uint8 reason;
};

void SendResult(WorldSession* to, uint8 result, ObjectGuid flightMaster, uint32 dest,
    std::vector<Row> const& rows)
{
    if (!to->IsSuiCapable())
        return;
    WorldPacket data(SMSG_SUI_PARTY_TAXI_RESULT, 1 + 8 + 4 + 1 + rows.size() * 9);
    data << uint8(result);
    data << uint64(flightMaster.GetRawValue());
    data << uint32(dest);
    data << uint8(rows.size());
    for (Row const& row : rows)
    {
        data << uint64(row.guid.GetRawValue());
        data << uint8(row.reason);
    }
    to->SendPacket(&data);
}

AiBotAI* BotAiOf(Player* bot)
{
    return bot ? dynamic_cast<AiBotAI*>(bot->AI()) : nullptr;
}

/// 0 = may board, else a Reason. Mirrors the gates Player::ActivateTaxiPathTo
/// applies (the same boarding cube, node knowledge, first-hop fare) so the
/// preview the client shows is the verdict the flight would give.
uint8 Eligibility(Player* member, Creature const* npc, TaxiNodesEntry const* source,
    std::vector<uint32> const& nodes, uint32 firstHopCost)
{
    if (!member->IsInWorld() || !member->IsAlive())
        return REASON_BUSY;
    if (member->IsSuiTacticallyFrozen())
        return REASON_BUSY;
    if (member->IsTaxiFlying())
        return REASON_IN_FLIGHT;
    if (member->GetMapId() != npc->GetMapId())
        return REASON_OTHER_MAP;
    if (member->IsInCombat() || member->IsNonMeleeSpellCasted(false) || member->IsBeingTeleported())
        return REASON_BUSY;
    // The boarding range ActivateTaxiPathTo enforces against the node position.
    float const dx = source->x - member->GetPositionX();
    float const dy = source->y - member->GetPositionY();
    float const dz = source->z - member->GetPositionZ();
    float const limit = (2 * INTERACTION_DISTANCE) * (2 * INTERACTION_DISTANCE) * (2 * INTERACTION_DISTANCE);
    if (dx * dx + dy * dy + dz * dz > limit)
        return REASON_TOO_FAR;
    if (!member->IsTaxiCheater())
        for (uint32 node : nodes)
            if (!member->GetTaxi().IsTaximaskNodeKnown(node))
                return REASON_UNKNOWN_NODE;
    uint32 const cost = uint32(firstHopCost * member->GetReputationPriceDiscount(npc, true) + 0.5f);
    if (member->GetMoney() < cost)
        return REASON_NO_MONEY;
    return 0;
}

} // namespace

void HandlePartyTaxi(WorldSession* session, uint8 flags, ObjectGuid flightMaster,
    std::vector<uint32> const& nodes)
{
    // Only MSUIClient speaks this opcode.
    session->SetSuiCapable(true);
    Player* requester = session->GetPlayer();
    if (!requester || !requester->IsInWorld() || session->GetBot())
        return;

    uint32 const dest = nodes.size() >= 2 ? nodes.back() : 0;
    std::vector<Row> rows;
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(session))
    {
        SendResult(session, RESULT_DENIED, flightMaster, dest, rows);
        return;
    }
    if (nodes.size() < 2 || nodes.size() > MAX_NODES)
    {
        SendResult(session, RESULT_DENIED, flightMaster, dest, rows);
        return;
    }

    // The flight master must be reachable from the body the session acts as:
    // the driven bot when commanding one from the sky, else the own character.
    Player* anchor = session->GetSuiActor();
    Creature* npc = anchor->GetNPCIfCanInteractWith(flightMaster, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!npc)
    {
        SendResult(session, RESULT_DENIED, flightMaster, dest, rows);
        return;
    }

    TaxiNodesEntry const* source = sObjectMgr.GetTaxiNodeEntry(nodes[0]);
    if (!source || source->map_id != npc->GetMapId())
    {
        SendResult(session, RESULT_NO_PATH, flightMaster, dest, rows);
        return;
    }
    uint32 firstPath = 0, firstCost = 0;
    sObjectMgr.GetTaxiPath(nodes[0], nodes[1], firstPath, firstCost);
    bool routed = firstPath != 0;
    for (size_t i = 1; routed && i + 1 < nodes.size(); ++i)
    {
        uint32 path = 0, cost = 0;
        sObjectMgr.GetTaxiPath(nodes[i], nodes[i + 1], path, cost);
        routed = path != 0;
    }
    if (!routed)
    {
        SendResult(session, RESULT_NO_PATH, flightMaster, dest, rows);
        return;
    }

    // The flight party: the requester's own character first, then every group
    // member that is a bot FOR THIS HUMAN (companions of another human and other
    // real players are never touched — SuiCompanion::MayCommand is the law).
    std::vector<Player*> party;
    party.push_back(requester);
    if (Group* group = requester->GetGroup())
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || member == requester)
                continue;
            if (!member->GetSession() || !member->GetSession()->GetBot())
                continue;
            if (!SuiCompanion::MayCommand(requester, member))
                continue;
            party.push_back(member);
        }

    std::vector<Player*> flyers;
    for (Player* member : party)
    {
        uint8 const reason = Eligibility(member, npc, source, nodes, firstCost);
        if (reason)
            rows.push_back({ member->GetObjectGuid(), reason });
        else
            flyers.push_back(member);
    }

    bool const confirmed = (flags & FLAG_CONFIRMED) != 0;
    if (!rows.empty() && !confirmed)
    {
        SendResult(session, RESULT_CONFIRM_NEEDED, flightMaster, dest, rows);
        return;
    }

    for (Player* member : flyers)
    {
        // The stock activation refuses a mounted rider; the fleet's own TAKE_FLIGHT
        // dismounts first, so do the same for every boarding member.
        if (member->IsMounted())
            member->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
        if (AiBotAI* ai = BotAiOf(member))
        {
            // Whatever leg the AI was walking ends here: the flight generator must
            // sit on a quiet motion master, and a live journey would resume walking
            // to its old goal the moment the flight lands.
            member->StopMoving();
            member->GetMotionMaster()->Clear(false, true);
            member->GetMotionMaster()->MoveIdle();
            ai->SuiAbandonJourney();
        }
        if (!member->ActivateTaxiPathTo(nodes, npc))
            rows.push_back({ member->GetObjectGuid(), REASON_REFUSED });
    }

    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "[SUI-TAXI] %s: party flight %u -> %u via %s, %u boarded, %u left behind%s",
        requester->GetName(), nodes[0], dest, npc->GetName(),
        uint32(party.size() - rows.size()), uint32(rows.size()), confirmed ? " (confirmed)" : "");
    SendResult(session, RESULT_FLYING, flightMaster, dest, rows);
}

} // namespace SuiTaxi

// ── Opcode entry ─────────────────────────────────────────────────────────────

void WorldSession::HandleSuiPartyTaxiOpcode(WorldPackets::SuiControl::PartyTaxi const& packet)
{
    if (!packet.exactSize)
        return;
    SuiTaxi::HandlePartyTaxi(this, packet.flags, packet.flightMaster, packet.nodes);
}
