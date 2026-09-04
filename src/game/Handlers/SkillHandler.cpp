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
#include "Opcodes.h"
#include "Log.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "UpdateMask.h"
#include "Anticheat.h"
#include "SuiPossess.h"      // [SUI] GetSuiActor + ResnapshotControlled: talents for a possessed companion
#include "SuiTacticalFreeze.h"

void WorldSession::HandleLearnTalentOpcode(WorldPackets::Skill::LearnTalent const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this))
        return;

    // [SUI] While the commander drives a possessed companion the talent goes to THAT body
    // (owner feedback 2026-09-03: "modify talent builds without logging out/in to other
    // characters"). The bot has no client session, so its new spell and remaining points
    // are re-pushed to the commander the same way a bag edit is (ItemHandler).
    Player* pActor = GetSuiActor();
    pActor->LearnTalent(packet.talent_id, packet.requested_rank);
    if (pActor != _player)
        SuiPossess::ResnapshotControlled(this);
}

void WorldSession::HandleTalentWipeConfirmOpcode(WorldPackets::Skill::TalentWipeConfirm const& packet)
{
    if (SuiTacticalFreeze::IsSessionGameplayFrozen(this) ||
        SuiTacticalFreeze::IsInteractionTargetFrozen(this, packet.guid))
        return;

    // [SUI] the driven bot's talents are wiped and ITS purse pays; the confirm frame mirrors back from a gossip pick
    Player* pActor = GetSuiActor();

    Creature* unit = pActor->GetNPCIfCanInteractWith(packet.guid, UNIT_NPC_FLAG_TRAINER);
    if (!unit)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "WORLD: HandleTalentWipeConfirmOpcode - %s not found or you can't interact with him.", packet.guid.GetString().c_str());
        return;
    }

    // remove fake death
    if (pActor->HasUnitState(UNIT_STATE_FEIGN_DEATH))
        pActor->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

    if (!(pActor->ResetTalents()))
    {
        WorldPacket data(MSG_TALENT_WIPE_CONFIRM, 8 + 4);   //you have not any talent
        data << uint64(0);
        data << uint32(0);
        SendPacket(&data);
        return;
    }

    unit->CastSpell(_player, 14867, true);                  //spell: "Untalent Visual Effect"

    // [SUI] Owner-only facts of the driven bot changed: re-push the snapshot.
    if (pActor != _player)
        SuiPossess::ResnapshotControlled(this);
}

void WorldSession::HandleUnlearnSkillOpcode(WorldPackets::Skill::UnlearnSkill const& packet)
{
    SkillRaceClassInfoEntry const* rcEntry = GetSkillRaceClassInfo(packet.skillId, GetPlayer()->GetRace(), GetPlayer()->GetClass());
    if (!rcEntry || !(rcEntry->flags & SKILL_FLAG_UNLEARNABLE))
    {
        std::stringstream reason;
        reason << "Attempt to unlearn not unlearnable skill #" << packet.skillId;
        ProcessAnticheatAction("PassiveAnticheat", reason.str().c_str(), CHEAT_ACTION_LOG | CHEAT_ACTION_REPORT_GMS);
        return;
    }
    GetPlayer()->SetSkill(packet.skillId, 0, 0);
}
