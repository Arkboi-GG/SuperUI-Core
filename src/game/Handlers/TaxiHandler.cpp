/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Path.h"
#include "WaypointMovementGenerator.h"
#include "SuiPossess.h"      // [SUI] GetSuiActor: the driven bot takes the flight, the human rides along
#include "SuiTacticalFreeze.h"

// [SUI] Taxi routing (owner 2026-09-03: "if I'm driving the bot, I follow the bot on
// taxi — I stay in control"). The node queries, the map and both activations act as
// GetSuiActor(): the flight master is ranged from the driven body, the map shows ITS
// discovered nodes, ITS purse pays and IT flies. SMSG_ACTIVATETAXIREPLY leaves on the
// flyer's session (Player::ActivateTaxiPathTo) and is mirrored to the commander; the
// flight spline is a public monster-move the commander's client already receives, and
// it now lets that spline drive the controller while possessing. Possession is NOT
// released by a flight — the human may still hop to another member (Ctrl+Tab /
// Ctrl+Click) and the flyer lands under its own AI. SendDoFlight stays session-scoped:
// ActivateTaxiPathTo calls it on the FLYER's session, which is the right player.

void WorldSession::HandleTaxiNodeStatusQueryOpcode(WorldPackets::Taxi::TaxiNodeStatusQuery const& packet)
{
    SendTaxiStatus(packet.creatureGuidNearTaxi);
}

void WorldSession::SendTaxiStatus(ObjectGuid guid)
{
    // [SUI] the driven bot's team and discovered nodes
    Player* actor = GetSuiActor();

    // cheating checks
    Creature* unit = actor->GetMap()->GetCreature(guid);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WorldSession::SendTaxiStatus - %s not found or you can't interact with it.", guid.GetString().c_str());
        return;
    }

    uint32 curloc = sObjectMgr.GetNearestTaxiNode(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(), unit->GetMapId(), actor->GetTeam());

    // not found nearest
    if (curloc == 0)
        return;

    WorldPacket data(SMSG_TAXINODE_STATUS, 9);
    data << ObjectGuid(guid);
    data << uint8(actor->m_taxi.IsTaximaskNodeKnown(curloc) ? 1 : 0);
    SendPacket(&data);
}

void WorldSession::HandleTaxiQueryAvailableNodes(WorldPackets::Taxi::TaxiQueryAvailableNodes const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;

    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.guid))
        return;

    // [SUI] the driven bot must reach the flight master
    Player* actor = GetSuiActor();

    // cheating checks
    Creature* unit = actor->GetNPCIfCanInteractWith(packet.guid, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleTaxiQueryAvailableNodes - %s not found or you can't interact with him.", packet.guid.GetString().c_str());
        return;
    }

    // remove fake death
    if (actor->HasUnitState(UNIT_STATE_FEIGN_DEATH))
        actor->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

    // unknown taxi node case
    if (SendLearnNewTaxiNode(unit))
        return;

    // known taxi node case
    SendTaxiMenu(unit);
}

void WorldSession::SendTaxiMenu(Creature* unit)
{
    // [SUI] the driven bot's taximask
    Player* actor = GetSuiActor();

    // find current node
    uint32 curloc = sObjectMgr.GetNearestTaxiNode(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(), unit->GetMapId(), actor->GetTeam());

    if (curloc == 0)
        return;

    WorldPacket data(SMSG_SHOWTAXINODES, (4 + 8 + 4 + 8 * 4));
    data << uint32(1);
    data << unit->GetObjectGuid();
    data << uint32(curloc);
    actor->m_taxi.AppendTaximaskTo(data, actor->IsTaxiCheater());
    SendPacket(&data);
}

void WorldSession::SendDoFlight(uint32 mountDisplayId, uint32 path, uint32 pathNode)
{
    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_FEIGN_DEATH))
        GetPlayer()->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

    while (GetPlayer()->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE)
        GetPlayer()->GetMotionMaster()->MovementExpired(false);

    if (mountDisplayId)
        GetPlayer()->Mount(mountDisplayId);

    // Check if it's a multi path
    if (!GetPlayer()->m_taxi.GetTaxiPath().empty())
        GetPlayer()->GetMotionMaster()->MoveTaxiFlight();
    else
        GetPlayer()->GetMotionMaster()->MoveTaxiFlight(path, pathNode);
}

bool WorldSession::SendLearnNewTaxiNode(Creature* unit)
{
    // [SUI] the driven bot discovers the node
    Player* actor = GetSuiActor();

    // find current node
    uint32 curloc = sObjectMgr.GetNearestTaxiNode(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(), unit->GetMapId(), actor->GetTeam());

    if (curloc == 0)
        return true;                                        // `true` send to avoid WorldSession::SendTaxiMenu call with one more curlock seartch with same false result.

    if (actor->m_taxi.SetTaximaskNode(curloc))
    {
        WorldPacket msg(SMSG_NEW_TAXI_PATH, 0);
        SendPacket(&msg);

        WorldPacket update(SMSG_TAXINODE_STATUS, 9);
        update << ObjectGuid(unit->GetObjectGuid());
        update << uint8(1);
        SendPacket(&update);

        return true;
    }
    else
        return false;
}

#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_9_4
void WorldSession::HandleActivateTaxiExpressOpcode(WorldPackets::Taxi::ActivateTaxiExpress const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;

    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.flightmasterGuid))
        return;

    // [SUI] the driven bot flies; the commander rides along
    Player* actor = GetSuiActor();

    Creature* npc = actor->GetNPCIfCanInteractWith(packet.flightmasterGuid, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!npc)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleActivateTaxiExpressOpcode - %s not found or you can't interact with it.", packet.flightmasterGuid.GetString().c_str());
        return;
    }

    if (packet.nodes.empty())
        return;

    actor->ActivateTaxiPathTo(packet.nodes, npc);
}
#endif

void WorldSession::HandleActivateTaxiOpcode(WorldPackets::Taxi::ActivateTaxi const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;

    if (SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.flightmasterGuid))
        return;

    // [SUI] the driven bot flies; the commander rides along
    Player* actor = GetSuiActor();

    std::vector<uint32> nodes { packet.node1, packet.node2 };

    Creature* npc = actor->GetNPCIfCanInteractWith(packet.flightmasterGuid, UNIT_NPC_FLAG_FLIGHTMASTER);
    if (!npc)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleActivateTaxiOpcode - %s not found or you can't interact with it.", packet.flightmasterGuid.GetString().c_str());
        return;
    }

    actor->ActivateTaxiPathTo(nodes, npc);
}
