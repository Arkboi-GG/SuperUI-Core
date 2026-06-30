/*
 * AiBotAIMain.cpp — Autonomous AI bot for VMaNGOS 1.12.1
 *
 * Split from the monolithic AiBotAI.cpp. THIS TU holds the engine entry points:
 * the lifecycle overrides (OnSessionLoaded / OnPlayerLogin / OnPacketReceived /
 * MovementInform) and the UpdateAI main loop that orchestrates everything else.
 *
 * The behaviour it drives lives in sibling TUs (all member functions of AiBotAI,
 * so they link across files transparently):
 *   combat   → AiBotAICombat.cpp     bridge → AiBotAIBridge.cpp
 *   movement → AiBotAIMovement.cpp    loot   → AiBotAILoot.cpp
 *   grind    → AiBotAIGrind.cpp
 */

#include "AiBotAIMain.h"
#include "AiBotAITeamPlay.h"   // [TEAMPLAY] ResolveCombatTarget — the group focus-fire resolver
#include "Player.h"
#include <cstring>
#include <cstdio>
#include "Group.h"
#include "CreatureAI.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "PlayerBotMgr.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "World.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "Chat.h"
#include "BattleGround.h"
#include "TargetedMovementGenerator.h"
#include "QuestDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Server/Packets/Channel.h"
#include "ChannelMgr.h"
#include "Bag.h"
#include "PathFinder.h"
#include "MoveMap.h"

// ============================================================
// LIFECYCLE
// ============================================================

void AiBotAI::OnPlayerLogin()
{
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] OnPlayerLogin for %s (guid %u)",
        me ? me->GetName() : "NULL",
        me ? me->GetGUIDLow() : 0);

    if (!m_initialized)
        me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);

        // Persist character to DB so it survives restarts
        me->SaveToDB();
}

bool AiBotAI::OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess)
{
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] OnSessionLoaded GUID=%u race=%u class=%u level=%u name='%s'",
        entry->playerGUID, m_spawnRace, m_spawnClass, m_spawnLevel,
        m_spawnName.c_str());

    // Check if this character already exists in the DB (restart vs first spawn)
    auto result = CharacterDatabase.PQuery(
        "SELECT 1 FROM `characters` WHERE `guid` = '%u'", entry->playerGUID);

    if (result)
    {
        // RESTART PATH: character exists with gear/spells/skills intact
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT] Existing character found for GUID=%u, using LoginPlayer",
            entry->playerGUID);
        sess->LoginPlayer(entry->playerGUID);
        return true;
    }

    // FIRST SPAWN PATH: no character exists, create fresh
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT] No existing character for GUID=%u, spawning fresh",
        entry->playerGUID);

    CharacterDatabase.PExecute("DELETE FROM `character_spell` WHERE `guid` = '%u'", entry->playerGUID);
    CharacterDatabase.PExecute("DELETE FROM `character_skills` WHERE `guid` = '%u'", entry->playerGUID);
    CharacterDatabase.PExecute("DELETE FROM `character_reputation` WHERE `guid` = '%u'", entry->playerGUID);
    CharacterDatabase.PExecute("DELETE FROM `character_homebind` WHERE `guid` = '%u'", entry->playerGUID);
    CharacterDatabase.PExecute("DELETE FROM `character_action` WHERE `guid` = '%u'", entry->playerGUID);
    sObjectMgr.DeletePlayerFromCache(entry->playerGUID);

    m_freshSpawn = true;

    if (!SpawnNewPlayer(sess, m_spawnClass, m_spawnRace, m_spawnMapId,
        m_spawnInstanceId, m_spawnX, m_spawnY, m_spawnZ, m_spawnO))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[AIBOT] SpawnNewPlayer FAILED for GUID=%u", entry->playerGUID);
        return false;
    }

    if (!m_spawnName.empty() && me)
    {
        std::string oldName = me->GetName();
        sObjectMgr.DeletePlayerFromCache(me->GetGUIDLow());
        me->SetName(m_spawnName);
        sObjectMgr.InsertPlayerInCache(me);
        entry->name = m_spawnName;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT] Renamed %s -> %s (GUID=%u)",
            oldName.c_str(), m_spawnName.c_str(), me->GetGUIDLow());
    }

    if (m_spawnLevel > 1 && me)
    {
        me->GiveLevel(m_spawnLevel);
        me->SetUInt32Value(PLAYER_XP, 0);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT] Set level %u for %s (GUID=%u)",
            m_spawnLevel, me->GetName(), me->GetGUIDLow());
    }

    return true;
}

void AiBotAI::OnPacketReceived(WorldPacket const* packet)
{
    // ── Session 31: Smart need/greed loot roll ──
    // CombatBotBaseAI auto-passes on SMSG_LOOT_START_ROLL.
    // We intercept first and make an intelligent decision using ScoreItem.
    if (packet->GetOpcode() == SMSG_LOOT_START_ROLL)
    {
        if (!me || !me->IsInWorld() || !me->GetGroup())
        {
            CombatBotBaseAI::OnPacketReceived(packet);
            return;
        }
 
        try
        {
            WorldPacket pkt(*packet);
 
            // Packet layout (from Group::SendLootStartRoll):
            //   uint64 lootedTargetGUID
            //   uint32 itemSlot
            //   uint32 itemId
            //   uint32 randomSuffix (unused)
            //   uint32 randomPropId
            //   uint32 countDown
            uint64 rawGuid;
            uint32 itemSlot, itemId, randomSuffix, randomPropId, countDown;
            pkt >> rawGuid >> itemSlot >> itemId >> randomSuffix >> randomPropId >> countDown;
 
            ObjectGuid lootedTarget(rawGuid);
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
 
            RollVote vote = ROLL_GREED; // default: greed (always better than pass for vendor gold)
 
            if (proto)
            {
                bool canEquip = false;
                bool isUpgrade = false;
 
                if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
                {
                    // Check class/race/level requirements
                    bool classOk = (proto->AllowableClass == 0 ||
                                    (proto->AllowableClass & me->GetClassMask()));
                    bool raceOk  = (proto->AllowableRace == 0 ||
                                    (proto->AllowableRace & me->GetRaceMask()));
                    bool levelOk = ((uint32)proto->RequiredLevel <= me->GetLevel());
 
                    if (classOk && raceOk && levelOk)
                    {
                        canEquip = true;
 
                        // Map InventoryType → equipment slot for ScoreItem comparison
                        uint8 targetSlot = 255;
                        switch (proto->InventoryType)
                        {
                            case INVTYPE_HEAD:           targetSlot = EQUIPMENT_SLOT_HEAD; break;
                            case INVTYPE_NECK:           targetSlot = EQUIPMENT_SLOT_NECK; break;
                            case INVTYPE_SHOULDERS:      targetSlot = EQUIPMENT_SLOT_SHOULDERS; break;
                            case INVTYPE_CHEST:
                            case INVTYPE_ROBE:           targetSlot = EQUIPMENT_SLOT_CHEST; break;
                            case INVTYPE_WAIST:          targetSlot = EQUIPMENT_SLOT_WAIST; break;
                            case INVTYPE_LEGS:           targetSlot = EQUIPMENT_SLOT_LEGS; break;
                            case INVTYPE_FEET:           targetSlot = EQUIPMENT_SLOT_FEET; break;
                            case INVTYPE_WRISTS:         targetSlot = EQUIPMENT_SLOT_WRISTS; break;
                            case INVTYPE_HANDS:          targetSlot = EQUIPMENT_SLOT_HANDS; break;
                            case INVTYPE_FINGER:         targetSlot = EQUIPMENT_SLOT_FINGER1; break;
                            case INVTYPE_TRINKET:        targetSlot = EQUIPMENT_SLOT_TRINKET1; break;
                            case INVTYPE_CLOAK:          targetSlot = EQUIPMENT_SLOT_BACK; break;
                            case INVTYPE_WEAPON:
                            case INVTYPE_2HWEAPON:
                            case INVTYPE_WEAPONMAINHAND: targetSlot = EQUIPMENT_SLOT_MAINHAND; break;
                            case INVTYPE_SHIELD:
                            case INVTYPE_WEAPONOFFHAND:
                            case INVTYPE_HOLDABLE:       targetSlot = EQUIPMENT_SLOT_OFFHAND; break;
                            case INVTYPE_RANGED:
                            case INVTYPE_THROWN:
                            case INVTYPE_RANGEDRIGHT:    targetSlot = EQUIPMENT_SLOT_RANGED; break;
                            default: break;
                        }
 
                        if (targetSlot != 255)
                        {
                            float newScore = ScoreItem(proto, targetSlot);
                            float oldScore = 0.0f;
 
                            Item* currentItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, targetSlot);
                            if (currentItem && currentItem->GetProto())
                                oldScore = ScoreItem(currentItem->GetProto(), targetSlot);
 
                            if (newScore > oldScore)
                                isUpgrade = true;
                        }
                    }
                }
 
                vote = (canEquip && isUpgrade) ? ROLL_NEED : ROLL_GREED;
 
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-ROLL] %s: [%s] (id=%u q=%u) → %s%s",
                    me->GetName(),
                    proto->Name1 ? proto->Name1 : "?",
                    itemId, proto->Quality,
                    vote == ROLL_NEED ? "NEED" : "GREED",
                    isUpgrade ? " (upgrade!)" : "");
            }
            else
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-ROLL] %s: unknown item %u → GREED",
                    me->GetName(), itemId);
            }
 
            // Submit vote via the same packet path as a real player clicking Need/Greed
            auto data = std::make_unique<WorldPackets::Loot::LootRoll>();
            data->lootedTarget = lootedTarget;
            data->itemSlot = itemSlot;
            data->rollType = vote;
            me->GetSession()->QueuePacket(std::move(data));
        }
        catch (...)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
                "[AIBOT-ROLL] %s: parse failed, falling back to base class (pass)",
                me->GetName());
            CombatBotBaseAI::OnPacketReceived(packet);
        }
 
        return; // Don't let base class auto-pass
    }
 
    // ── Existing: Intercept incoming chat messages (say, whisper, channel) ──
    if (packet->GetOpcode() == SMSG_MESSAGECHAT)
    {
        try
        {
            WorldPacket pkt(*packet); // copy so we can read
            uint8 chatType;
            uint32 lang;
            pkt >> chatType >> lang;
 
            if (chatType == CHAT_MSG_SAY || chatType == CHAT_MSG_WHISPER)
            {
                ObjectGuid senderGuid;
                pkt >> senderGuid;
 
                // SAY, YELL, PARTY have a SECOND copy of senderGuid
                if (chatType == CHAT_MSG_SAY || chatType == CHAT_MSG_YELL || chatType == CHAT_MSG_PARTY)
                {
                    ObjectGuid dupGuid;
                    pkt >> dupGuid;
                }
 
                uint32 textLen;
                pkt >> textLen;
                if (textLen > 0 && textLen < 512)
                {
                    std::string message;
                    pkt >> message;
 
                    std::string senderName = "Unknown";
                    if (Player* pSender = sObjectMgr.GetPlayer(senderGuid))
                        senderName = pSender->GetName();
 
                    const char* typeStr = (chatType == CHAT_MSG_WHISPER) ? "whisper" : "say";
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: CHAT_RECV [%s] from %s: %s",
                        me->GetName(), typeStr, senderName.c_str(), message.c_str());
                    SendChatRecvEvent(senderName.c_str(), message.c_str(), typeStr);
                }
            }
            else if (chatType == CHAT_MSG_CHANNEL)
            {
                std::string channelName;
                pkt >> channelName;
 
                uint32 playerRank;
                pkt >> playerRank;
 
                ObjectGuid senderGuid;
                pkt >> senderGuid;
 
                uint32 textLen;
                pkt >> textLen;
                if (textLen > 0 && textLen < 512)
                {
                    std::string message;
                    pkt >> message;
 
                    std::string senderName = "Unknown";
                    if (Player* pSender = sObjectMgr.GetPlayer(senderGuid))
                        senderName = pSender->GetName();
 
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT-BRIDGE] %s: CHAT_RECV [channel:%s] from %s: %s",
                        me->GetName(), channelName.c_str(), senderName.c_str(), message.c_str());
                    SendChatRecvEvent(senderName.c_str(), message.c_str(), "channel", channelName.c_str());
                }
            }
        }
        catch (...)
        {
            // Packet parse failed — not critical, just skip
        }
    }
 
    CombatBotBaseAI::OnPacketReceived(packet);
}

void AiBotAI::MovementInform(uint32 MovementType, uint32 Data)
{
    if (MovementType == POINT_MOTION_TYPE)
    {
        if (Data == AIBOT_POINT_WANDER)
        {
            // Arrived at wander destination — go idle, will wander again after timer
            me->GetMotionMaster()->MoveIdle();
        }
        else if (Data == AIBOT_POINT_STALEMATE_NUDGE)
        {
            // Stalemate hop landed — go idle; HandleCombatStalemate re-evaluates next tick.
            me->GetMotionMaster()->MoveIdle();
        }
        else if (Data == AIBOT_POINT_OVERPULL_FLEE)
        {
            // Retreat hop landed — go idle; HandleOverpullRetreat re-evaluates next tick.
            me->GetMotionMaster()->MoveIdle();
        }
        else if (Data == AIBOT_POINT_TASK_DEST)
        {
            // More chunks remaining in the current (possibly partial) leg?
            if (!m_pathWaypoints.empty() &&
                m_pathIndex < (uint32)m_pathWaypoints.size() - 1)
            {
                StartNextPathChunk();
                return;
            }

            // This leg's chunks are exhausted. If it was a PARTIAL leg that stopped
            // short of the true destination, re-query and walk the next leg —
            // continuation, no distance ceiling. stopCurrentMovement = false because
            // we're inside the motion callback (MovePoint is safe here; a full
            // StopMoving()/MotionMaster::Clear() is not).
            if (m_currentTask.type == TASK_MOVE_TO)
            {
                float dist = me->GetDistance2d(m_currentTask.x, m_currentTask.y);
                if (dist > 3.0f)
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-PATH] %s: leg exhausted %.0fyd from dest — continuing journey",
                        me->GetName(), dist);
                    ClearStoredPath();
                    MoveToDestination(m_currentTask.x, m_currentTask.y, m_currentTask.z, false);
                    return;
                }
            }

            // §4: enriched objective MOVE_TO reached the deep coord with no scan hit
            // (mobs were deeper than scan range during the walk) — grind here in place
            // instead of falsely reporting "arrived" (merged step: a TASK_COMPLETE here
            // would mean objective-done with zero kills). Bare MOVE_TO emits "arrived".
            if (m_currentTask.type == TASK_MOVE_TO && m_currentTask.creatureEntry != 0)
            {
                ConvertMoveToGrindInPlace();
                return;
            }

            // Path complete (arrived, or it was a short single-MovePoint path).
            // Stamp the exact arrival coord into the event so C# refreshes ctx.Pos NOW
            // instead of waiting up to one 5s STATE cycle — otherwise a just-arrived bot
            // still reads stale-far and the planner re-issues MOVE_TO instead of interacting.
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s arrived at task destination", me->GetName());
            char arrBuf[96];
            snprintf(arrBuf, sizeof(arrBuf), "MOVE_TO arrived|x=%.1f|y=%.1f|z=%.1f",
                     me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
            BridgeSendEvent("TASK_COMPLETE", arrBuf);
            m_currentTask.Clear();
            ClearStoredPath();
        }
        else if (Data == AIBOT_POINT_GRIND_PATROL)
        {
            me->GetMotionMaster()->MoveIdle();
            // Arrived at patrol point within grind area — will pick next target or patrol again
        }
    }
}

// ============================================================
// MAIN UPDATE LOOP — COMPLETE-METHOD REPLACEMENT
//
// Drop-in replacement for AiBotAI::UpdateAI in AiBotAIMain.cpp. Identical to the
// deployed overpull/§4/[GRAVE-SELFREZ] version EXCEPT the TASK_GRIND post-freeze block,
// which now SELF-UNSTICKS the objective grind instead of handing back GRIND_BLOCKED|
// overpull_dwell (the kobold-camp livelock fix).
//
// Why: a dense field of QUEST mobs vetoed every pull (OverpullGuard: >AIBOT_OVERPULL_SOLO
// within AIBOT_PULL_DENSITY_RADIUS). The objective grind then handed back overpull_dwell,
// which C# always answered with a forced single-kill detour + re-issue of the SAME
// objective → re-freeze on the SAME camp. One kill per ~3s roundtrip until the fail/
// deadline machinery shelved the quest and the bot wandered off to easier work, beside an
// ocean of valid mobs. Now BOTH grind modes self-unstick after AIBOT_GRIND_FREEZE_DWELL:
// SelectGrindTarget already returns the LEAST-clustered candidate, HandleOverpullRetreat is
// the in-combat backstop if too many pile on, and a genuinely unkillable field still trips
// the C# 120s no-kill deadline → durable shelve. The no_target handback (truly empty field)
// is UNCHANGED. Only the overpull_dwell objective handback is removed.
// ============================================================

void AiBotAI::UpdateAI(uint32 const diff)
{
    // Handle pending teleports from base class
    PlayerBotAI::UpdateAI(diff);
    m_updateTimer.Update(diff);
    if (m_updateTimer.Passed())
        m_updateTimer.Reset(AIBOT_UPDATE_INTERVAL);
    else
        return;

    if (!me->IsInWorld() || me->IsBeingTeleported())
        return;

    // Decrement wander/patrol timer
    if (m_wanderTimer > AIBOT_UPDATE_INTERVAL)
        m_wanderTimer -= AIBOT_UPDATE_INTERVAL;
    else
        m_wanderTimer = 0;

    // §4 approach-scan throttle
    if (m_approachScanTimer > AIBOT_UPDATE_INTERVAL)
        m_approachScanTimer -= AIBOT_UPDATE_INTERVAL;
    else
        m_approachScanTimer = 0;

    // [ADDED] Combat-stalemate ignore set: tick down per-guid cooldowns
    for (auto it = m_combatIgnore.begin(); it != m_combatIgnore.end(); )
    {
        if (it->second <= AIBOT_UPDATE_INTERVAL)
            it = m_combatIgnore.erase(it);
        else { it->second -= AIBOT_UPDATE_INTERVAL; ++it; }
    }

    // --- Bridge: connect + recv + periodic state ---
    if (!m_bridgeConnected)
    {
        if (m_bridgeReconnectTimer <= AIBOT_UPDATE_INTERVAL)
        {
            m_bridgeReconnectTimer = 0;
            BridgeConnect();
        }
        else
        {
            m_bridgeReconnectTimer -= AIBOT_UPDATE_INTERVAL;
        }
    }

    if (m_bridgeConnected)
    {
        if (!m_bridgeHelloSent && m_initialized)
            BridgeSendHello();

        BridgeRecv();

        if (m_bridgeHelloSent)
        {
            if (m_bridgeStateTimer <= AIBOT_UPDATE_INTERVAL)
            {
                m_bridgeStateTimer = BRIDGE_STATE_INTERVAL;
                BridgeSendState();
            }
            else
            {
                m_bridgeStateTimer -= AIBOT_UPDATE_INTERVAL;
            }
        }

        BridgeFlush();   // Session 36: drain bytes deferred by a prior partial/blocked write
    }

    // One-time log on first successful update tick
    if (!m_loggedFirstUpdate)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s (guid %u) UpdateAI active - class %u, level %u, zone %u, map %u",
            me->GetName(), me->GetGUIDLow(), me->GetClass(), me->GetLevel(),
            me->GetZoneId(), me->GetMapId());
        m_loggedFirstUpdate = true;
        me->SetSaveDisabled(false);
        me->SaveToDB();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s (guid %u) saved to DB", me->GetName(), me->GetGUIDLow());
    }

    // --- Initialization (once, on first update after login) ---
    if (!m_initialized)
    {
        if (m_freshSpawn)
        {
            LearnPremadeSpecForClass();
            // Only give starting gear, not premade BG gear
            AutoEquipGear(PLAYER_BOT_AUTO_EQUIP_STARTING_GEAR);
        }

        if (m_role == ROLE_INVALID)
            AutoAssignRole();

        ResetSpellData();
        PopulateSpellData();
        AddAllSpellReagents();
        me->UpdateSkillsToMaxSkillsForLevel();
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING);
        SummonPetIfNeeded();
        me->SetHealthPercent(100.0f);
        me->SetPowerPercent(me->GetPowerType(), 100.0f);

        if (urand(0, 1))
        {
            me->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_HELM);
            me->ToggleFlag(PLAYER_FLAGS, PLAYER_FLAGS_HIDE_CLOAK);
        }

        uint32 newzone, newarea;
        me->GetZoneAndAreaId(newzone, newarea);
        me->UpdateZone(newzone, newarea);

        // Join zone-specific General and Trade channels so bots can hear channel chat
        // Channel names are zone-dependent: "General - Elwynn Forest", "Trade - Stormwind City", etc.
        if (me->GetSession())
        {
            const AreaEntry* zoneEntry = AreaEntry::GetById(newzone);
            if (zoneEntry && zoneEntry->Name)
            {
                std::string zoneName = zoneEntry->Name;

                WorldPackets::Channel::JoinChannel joinGeneral;
                joinGeneral.channelName = "General - " + zoneName;
                joinGeneral.channelPassword = "";
                me->GetSession()->HandleJoinChannelOpcode(joinGeneral);

                WorldPackets::Channel::JoinChannel joinTrade;
                joinTrade.channelName = "Trade - " + zoneName;
                joinTrade.channelPassword = "";
                me->GetSession()->HandleJoinChannelOpcode(joinTrade);

                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s joined General and Trade channels for zone %s",
                    me->GetName(), zoneName.c_str());
            }
            else
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s could not resolve zone name for zone %u, skipping channel join",
                    me->GetName(), newzone);
            }
        }

        m_initialized = true;
        m_lastKnownLevel = me->GetLevel();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s initialized - class %u, level %u, role %u",
            me->GetName(), me->GetClass(), me->GetLevel(), m_role);
        return;
    }

    // --- Death handling: ghost at corpse, wait for C# RESURRECT (or self-rez at a graveyard) ---
    if (me->IsDead())
    {
        if (!m_wasDead)
        {
            m_wasDead = true;

            float deathX = me->GetPositionX();
            float deathY = me->GetPositionY();
            float deathZ = me->GetPositionZ();
            uint32 deathMap = me->GetMapId();

            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s died at (%.1f, %.1f, %.1f) map=%u — ghosting at corpse, waiting for C# RESURRECT",
                me->GetName(), deathX, deathY, deathZ, deathMap);

            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType())
                StopMoving();

            m_currentTask.Clear();

            // Clean up any existing corpse from a previous death
            if (Corpse* oldCorpse = me->GetCorpse())
                me->SpawnCorpseBones();

            // Ghost form — bot becomes translucent spirit, corpse drops
            me->BuildPlayerRepop();
            // NO RepopAtGraveyard — ghost stays right here at the death position

            char deathData[160];
            snprintf(deathData, sizeof(deathData),
                "x=%.1f|y=%.1f|z=%.1f|map=%u|attackers=%u",
                deathX, deathY, deathZ, deathMap, m_lastAttackerCount);
            BridgeSendEvent("DEATH", deathData);

            return;
        }

        // [GRAVE-SELFREZ] Graveyard self-rez — the firing half of the zone-0 escape.
        if (m_pendingGraveyardRez)
        {
            if (m_graveRezWaitMs > AIBOT_UPDATE_INTERVAL)
                m_graveRezWaitMs -= AIBOT_UPDATE_INTERVAL;
            else
                m_graveRezWaitMs = 0;

            float distToGrave = me->GetDistance2d(m_graveRezX, m_graveRezY);
            bool landed   = (me->GetMapId() == m_graveRezMap) && (distToGrave < 25.0f);
            bool timedOut = (m_graveRezWaitMs == 0);

            if (landed || timedOut)
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s graveyard self-rez %s at (%.1f, %.1f) (dist=%.1f)",
                    me->GetName(), landed ? "confirmed" : "TIMEOUT",
                    me->GetPositionX(), me->GetPositionY(), distToGrave);

                m_pendingGraveyardRez = false;
                m_graveRezWaitMs = 0;

                me->ResurrectPlayer(0.5f);
                me->CombatStop(true);   // never resume rez in-combat (a ghost pulled nothing anyway)
                me->SpawnCorpseBones();

                BridgeSendEvent("RESPAWN", "");
                BridgeSendState();
                return;
            }

            return;
        }

        // Subsequent ticks while dead: keep bridge alive, wait for RESURRECT.
        return;
    }
    else
    {
        if (m_wasDead)
        {
            m_wasDead = false;
            SummonPetIfNeeded();
            return;
        }
    }

    // [OVERPULL] Peak melee attackers this combat — stamped on the DEATH event above.
    if (me->IsInCombat())
    {
        uint32 atk = (uint32)me->GetAttackers().size();
        if (atk > m_lastAttackerCount)
            m_lastAttackerCount = atk;
    }
    else
        m_lastAttackerCount = 0;


    // --- TASK_TAXI: in-flight — skip ALL behavior until we land ---
    if (m_currentTask.type == TASK_TAXI)
    {
        if (me->GetTaxi().empty() && !me->HasUnitState(UNIT_STATE_TAXI_FLIGHT))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s: flight complete (arrived at node %u)",
                me->GetName(), m_currentTask.taxiDestNode);
            BridgeSendEvent("FLIGHT_COMPLETE", "");
            m_currentTask.Clear();
            // Don't return — fall through to normal idle behavior this tick
        }
        else
        {
            return; // still flying — don't wander, fight, eat, buff, etc.
        }
    }

    // --- Level-up detection ---
    if (m_lastKnownLevel > 0 && me->GetLevel() > m_lastKnownLevel)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] %s leveled up: %u -> %u",
            me->GetName(), m_lastKnownLevel, me->GetLevel());
        SendLevelUpEvent(me->GetLevel());
        m_lastKnownLevel = me->GetLevel();

        // Re-learn spells and update skills for new level
        PopulateSpellData();
        me->UpdateSkillsToMaxSkillsForLevel();
    }

    // --- Auto-loot timer ---
    if (m_lootTimer > 0)
    {
        m_lootTimer -= (int32)AIBOT_UPDATE_INTERVAL;
        if (m_lootTimer <= 0)
        {
            m_lootTimer = 0;
            if (!m_lootTargetGuid.IsEmpty())
            {
                DoAutoLoot(m_lootTargetGuid);
                m_lootTargetGuid.Clear();
            }
        }
    }

    // --- CC break ---
    if (me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
    {
        BreakCrowdControlEffects();
        return;
    }

    // --- Auto-repeat spell handling (Hunter auto shot) ---
    if (me->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
    {
        if (!me->GetVictim())
            me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
        else if (me->GetClass() == CLASS_HUNTER)
        {
            if (me->GetCombatDistance(me->GetVictim()) < 8.0f)
                me->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, true);
            else
                UpdateInCombatAI_Hunter();
        }

        return;
    }

    if (me->IsNonMeleeSpellCasted(false, false, true))
        return;

    if (me->GetTargetGuid() == me->GetObjectGuid())
        me->ClearTarget();

    Unit* pVictim = me->GetVictim();

    // Prevent chasing stealthed target
    if (pVictim && !pVictim->IsVisibleForOrDetect(me, me, false))
    {
        me->AttackStop();
        me->ClearTarget();
        me->StopMoving();
        if (pVictim = SelectAttackTarget(pVictim))
            AttackStart(pVictim);
        return;
    }

    // --- Out of combat: eat/drink ---
    if (!me->IsInCombat())
    {
        bool hasActiveTask = (m_currentTask.type == TASK_MOVE_TO ||
                              m_currentTask.type == TASK_GRIND);
        bool criticalHP = me->GetHealthPercent() < 40.0f;
        bool criticalMana = (me->GetPowerType() == POWER_MANA) &&
                            (me->GetPowerPercent(POWER_MANA) < 20.0f);

        if (!hasActiveTask || criticalHP || criticalMana)
        {
            if (DrinkAndEat())
                return;
        }
    }

    if (me->GetStandState() != UNIT_STAND_STATE_STAND)
        me->SetStandState(UNIT_STAND_STATE_STAND);

    if (me->GetSheath() == SHEATH_STATE_UNARMED && !me->IsMounted())
        me->SetSheath(SHEATH_STATE_MELEE);

    // --- Out of combat behavior ---
    if (!me->IsInCombat())
    {
        if (CheckForUnreachableTarget())
            return;

        UpdateOutOfCombatAI();

        if (m_isBuffing)
            return;

        // Can enter combat from UpdateOutOfCombatAI().
        if (me->IsInCombat())
            return;

        if (me->IsNonMeleeSpellCasted())
            return;


        // --- Kill detection (must run before combat/OOC branching) ---
        if (!pVictim && m_lastVictimEntry != 0)
        {
            // Victim pointer cleared = mob died or despawned. Fire kill event.
            // --- Tapped check: only process kills we actually tagged ---
            Creature* pKillCreature = me->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, m_lastVictimEntry, m_lastVictimGuidLow));
            if (pKillCreature && pKillCreature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED) && !pKillCreature->IsTappedBy(me))
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: mob (entry=%u guid=%u) tapped by another — skipping kill credit",
                    me->GetName(), m_lastVictimEntry, m_lastVictimGuidLow);
                m_lastVictimEntry = 0;
                m_lastVictimGuidLow = 0;
            }
            else
            {
            SendKillEvent(m_lastVictimEntry, m_lastVictimGuidLow);
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT] %s: kill detected (entry=%u guid=%u)",
                me->GetName(), m_lastVictimEntry, m_lastVictimGuidLow);

            // Queue auto-loot with humanization delay
            Creature* victim = me->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, m_lastVictimEntry, m_lastVictimGuidLow));
            if (victim && victim->IsDead())
            {
                m_lootTargetGuid = victim->GetObjectGuid();
                m_lootTimer = urand(1000, 2500);
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-LOOT] %s: loot timer started (%dms) for %s (entry=%u)",
                    me->GetName(), m_lootTimer, victim->GetName(), m_lastVictimEntry);
            }
            else
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-LOOT] %s: could not find dead creature for loot (entry=%u guid=%u)",
                    me->GetName(), m_lastVictimEntry, m_lastVictimGuidLow);
            }

            if (m_currentTask.type == TASK_GRIND)
            {
                bool matches = (m_currentTask.creatureEntry == 0 ||
                                m_lastVictimEntry == m_currentTask.creatureEntry);
                if (matches)
                {
                    m_currentTask.killCount++;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT] %s: GRIND kill %d/%d (entry=%u)",
                        me->GetName(), m_currentTask.killCount, m_currentTask.killGoal, m_lastVictimEntry);
                    if (m_currentTask.killGoal > 0 &&
                        m_currentTask.killCount >= m_currentTask.killGoal)
                    {
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT] %s: GRIND task complete (%d/%d kills)",
                            me->GetName(), m_currentTask.killCount, m_currentTask.killGoal);
                        BridgeSendEvent("TASK_COMPLETE", "GRIND finished");
                        m_currentTask.Clear();
                    }
                }
            }
            m_lastVictimEntry = 0;
            m_lastVictimGuidLow = 0;
            }
        }
        else if (pVictim && pVictim->IsCreature())
        {
            m_lastVictimEntry = static_cast<Creature*>(pVictim)->GetEntry();
            m_lastVictimGuidLow = pVictim->GetGUIDLow();
        }

        // --- TASK_GRIND: proactive pull or patrol ---
        if (m_currentTask.type == TASK_GRIND)
        {
            // An entry==0 grind is "kill whatever's nearest": the indefinite filler (Goal.Grinding,
            // killGoal==0) OR the C# unstick detour (killGoal==1). It is logged distinctly below, but
            // BOTH grind modes now self-unstick a dense field (see the freeze-escape block) — the
            // objective grind no longer hands overpull_dwell back to C#.
            bool const fillerOrDetour = (m_currentTask.creatureEntry == 0);

            if (Unit* pGrindTarget = SelectGrindTarget())
            {
                if (OverpullGuard(pGrindTarget))
                {
                    ++m_grindFreezeStreak;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-OVERPULL] %s: holding (%u/%u) — target %s in %u-deep cluster (solo cap %u)%s",
                        me->GetName(), m_grindFreezeStreak, AIBOT_GRIND_FREEZE_DWELL,
                        pGrindTarget->GetName(),
                        CountNearbyHostiles(pGrindTarget, AIBOT_PULL_DENSITY_RADIUS), AIBOT_OVERPULL_SOLO,
                        " [will self-unstick]");

                    if (m_grindFreezeStreak >= AIBOT_GRIND_FREEZE_DWELL)
                    {
                        m_grindFreezeStreak = 0;

                        // Self-unstick BOTH grind modes. The objective grind USED to hand back
                        // GRIND_BLOCKED|overpull_dwell so C# could "resync + re-derive" — but the brain
                        // ALWAYS answers overpull_dwell with a forced single-kill detour, then re-issues
                        // the SAME objective, which re-freezes against the SAME dense field. Net effect:
                        // a quest-mob camp gets ground one kill per ~3s C# roundtrip, and once the fail/
                        // deadline machinery trips, the bot shelves the quest and wanders off to easier
                        // work — beside an ocean of perfectly valid mobs (the kobold-camp livelock). A
                        // dense field of QUEST mobs is exactly where we want to grind, so take the pull
                        // right here, same as the filler/detour path already does: SelectGrindTarget
                        // handed us the LEAST-clustered candidate (its density score), HandleOverpullRetreat
                        // stays the in-combat backstop if too many pile on, and a field that genuinely
                        // can't be killed still trips the C# 120s no-kill deadline → durable shelve.
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT-OVERPULL] %s: freeze-escape — self-unstick pull of %s (bypassing cap, %s)",
                            me->GetName(), pGrindTarget->GetName(),
                            fillerOrDetour ? "filler/detour" : "objective");
                        AttackStart(pGrindTarget);
                        return;
                    }

                    DoGrindPatrol();
                    return;
                }

                // Pull cleared the cap → engage. Reset the freeze streak.
                m_grindFreezeStreak = 0;
                AttackStart(pGrindTarget);
                return;
            }

            // No valid target this tick. Only an OBJECTIVE grind (entry!=0) hands back (quest mobs all
            // dead/tapped → C# resyncs + detours). An entry==0 grind just patrols: a filler with no mobs
            // is GrindPlanner's no-kills→reselect job, and a detour with no mob rides its own WAIT deadline.
            if (m_currentTask.creatureEntry != 0)
            {
                if (++m_grindFreezeStreak >= AIBOT_GRIND_FREEZE_DWELL)
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-OVERPULL] %s: freeze-escape — no valid target %u ticks, signaling GRIND_BLOCKED",
                        me->GetName(), m_grindFreezeStreak);
                    m_grindFreezeStreak = 0;
                    char ev[160];
                    snprintf(ev, sizeof(ev),
                        "x=%.1f|y=%.1f|z=%.1f|entry=%u|reason=no_target",
                        m_currentTask.x, m_currentTask.y, m_currentTask.z,
                        m_currentTask.creatureEntry);
                    BridgeSendEvent("GRIND_BLOCKED", ev);
                }
            }

            DoGrindPatrol();
            return;
        }

       // --- TASK_MOVE_TO: resume movement after interruption ---
        if (m_currentTask.type == TASK_MOVE_TO)
        {
            // ── §4 approach scan ──
            if (m_currentTask.creatureEntry != 0 && m_approachScanTimer == 0)
            {
                m_approachScanTimer = urand(2000, 3000);
                if (Unit* pMob = ScanApproachTarget())
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-PATH] %s: approach scan hit %s (entry=%u, %.0fyd) — handoff to GRIND",
                        me->GetName(), pMob->GetName(), m_currentTask.creatureEntry, me->GetDistance(pMob));
                    ConvertMoveToGrindInPlace();
                    // [OVERPULL] Same gate as the grind dispatch: convert to a local grind but
                    // don't dive a dense pack solo. The grind dispatch re-gates next tick.
                    if (!OverpullGuard(pMob))
                        AttackStart(pMob);
                    return;
                }
            }

            // Still actively walking — let the motion generator finish
            if (me->IsMoving())
                return;

            // Not moving. Either we arrived or we got interrupted.
            float dist = me->GetDistance2d(m_currentTask.x, m_currentTask.y);

            if (dist > 3.0f)
            {
                // Haven't arrived — resume movement.
                if (!m_pathWaypoints.empty() &&
                    m_pathIndex < (uint32)m_pathWaypoints.size() - 1)
                {
                    // Resume the current leg's remaining chunks
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-PATH] %s: resuming chunked path from waypoint %u/%u",
                        me->GetName(), m_pathIndex, (uint32)m_pathWaypoints.size());
                    StartNextPathChunk();
                }
                else
                {
                    // Current leg exhausted (or interrupted with no stored path) —
                    // compute the next leg toward the true dest and walk it.
                    MoveToDestination(m_currentTask.x, m_currentTask.y, m_currentTask.z);
                }
            }
            else
            {
                // Close enough to the destination.
                // §4: an enriched objective MOVE_TO arrived at the deep coord with no
                // scan hit — grind here in place rather than emit a false "arrived"
                // TASK_COMPLETE (merged step: that would mean objective-done, zero kills).
                if (m_currentTask.creatureEntry != 0)
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT] %s: objective MOVE_TO auto-arrived (dist=%.1f) — GRIND in place",
                        me->GetName(), dist);
                    ConvertMoveToGrindInPlace();
                }
                else
                {
                    // Stamp the exact arrival coord so C# refreshes ctx.Pos immediately
                    // (no 5s STATE-cycle stale read driving a needless MOVE_TO re-issue).
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT] %s: MOVE_TO auto-arrived (dist=%.1f, threshold=3.0)",
                        me->GetName(), dist);
                    char arrBuf[96];
                    snprintf(arrBuf, sizeof(arrBuf), "MOVE_TO arrived|x=%.1f|y=%.1f|z=%.1f",
                             me->GetPositionX(), me->GetPositionY(), me->GetPositionZ());
                    BridgeSendEvent("TASK_COMPLETE", arrBuf);
                    m_currentTask.Clear();
                    ClearStoredPath();
                }
            }
            return;
        }

        // --- Default idle wander ---
        DoRandomWander();
        return;
    }

    // [ADDED] Combat stalemate breaker — runs only while in combat.
    if (HandleCombatStalemate())
        return;

    // [OVERPULL] Overpull retreat — bail out of a fight where more than the cap are on us.
    if (HandleOverpullRetreat())
        return;

    // Cache victim info before combat system clears it
    if (pVictim && pVictim->IsCreature())
    {
        m_lastVictimEntry = static_cast<Creature*>(pVictim)->GetEntry();
        m_lastVictimGuidLow = pVictim->GetGUIDLow();
    }

    // --- Tap-respect: drop a mob the server says belongs to a non-group player ---
    if (pVictim && pVictim->IsCreature() && !m_combatDirective.IsActive())
    {
        Creature* pVicCre = static_cast<Creature*>(pVictim);
        if (pVicCre->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED) &&
            !pVicCre->IsTappedBy(me))
        {
            // Is the tapper in my group? If so it's a focus-fire assist, not a steal.
            bool tapperInMyGroup = false;
            if (Group* pGroup = me->GetGroup())
            {
                if (Player* pTapper = pVicCre->GetLootRecipient())
                    tapperInMyGroup = pGroup->IsMember(pTapper->GetObjectGuid());
            }

            if (!tapperInMyGroup)
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: victim %u (guid=%u) tapped by another — disengaging (not mine)",
                    me->GetName(), pVicCre->GetEntry(), pVicCre->GetGUIDLow());

                me->AttackStop();
                if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                    StopMoving();
                me->ClearTarget();

                // Short-ignore so Select*Target / SelectGrindTarget don't instantly re-acquire
                // this same tapped mob from the hostile-ref list next tick.
                m_combatIgnore[pVicCre->GetGUIDLow()] = AIBOT_TAPPED_IGNORE_MS;
                return;
            }
        }
    }

    // --- In combat: target validation + switch ---
    if (!pVictim || !IsValidHostileTarget(pVictim) ||
        !pVictim->IsWithinDist(me, VISIBILITY_DISTANCE_SMALL))
    {
        bool victimDied = (pVictim && pVictim->IsDead() && pVictim->IsCreature());
        if (!victimDied && !pVictim && m_lastVictimEntry != 0 && !me->IsInCombat())
            victimDied = true;

        if (victimDied)
        {
            uint32 killedEntry = pVictim ? static_cast<Creature*>(pVictim)->GetEntry() : m_lastVictimEntry;
            uint32 killedGuid = pVictim ? pVictim->GetGUIDLow() : m_lastVictimGuidLow;
            // --- Tapped check: only process kills we actually tagged ---
            Creature* pKillCreature2 = pVictim ? static_cast<Creature*>(pVictim) : me->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, m_lastVictimEntry, m_lastVictimGuidLow));
            if (pKillCreature2 && pKillCreature2->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED) && !pKillCreature2->IsTappedBy(me))
            {
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT] %s: mob (entry=%u guid=%u) tapped by another — skipping kill credit (combat path)",
                    me->GetName(), killedEntry, killedGuid);
                m_lastVictimEntry = 0;
                m_lastVictimGuidLow = 0;
            }
            else
            {
            SendKillEvent(killedEntry, killedGuid);

            // Queue auto-loot with humanization delay
            Creature* deadCreature = pVictim ? static_cast<Creature*>(pVictim) : me->GetMap()->GetCreature(ObjectGuid(HIGHGUID_UNIT, m_lastVictimEntry, m_lastVictimGuidLow));
            if (deadCreature && deadCreature->IsDead())
            {
                m_lootTargetGuid = deadCreature->GetObjectGuid();
                m_lootTimer = urand(1000, 2500);
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-LOOT] %s: loot timer started (%dms) for entry=%u (in-combat path)",
                    me->GetName(), m_lootTimer, killedEntry);
            }

            if (m_currentTask.type == TASK_GRIND)
            {
                bool matches = (m_currentTask.creatureEntry == 0 ||
                                killedEntry == m_currentTask.creatureEntry);
                if (matches)
                {
                    m_currentTask.killCount++;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT] %s: GRIND kill %d/%d (entry=%u)",
                        me->GetName(), m_currentTask.killCount, m_currentTask.killGoal, killedEntry);
                    if (m_currentTask.killGoal > 0 &&
                        m_currentTask.killCount >= m_currentTask.killGoal)
                    {
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT] %s: GRIND task complete (%d/%d kills)",
                            me->GetName(), m_currentTask.killCount, m_currentTask.killGoal);
                        BridgeSendEvent("TASK_COMPLETE", "GRIND finished");
                        m_currentTask.Clear();
                    }
                }
            }

            m_lastVictimEntry = 0;
            m_lastVictimGuidLow = 0;
            }
        }

        if (Unit* pNewVictim = SelectAttackTarget(pVictim))
        {
            if (pVictim != pNewVictim)
            {
                AttackStart(pNewVictim);
                return;
            }
        }

        if (me->GetVictim() &&
           (me != me->GetVictim()->GetVictim()))
        {
            me->AttackStop(false);
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE)
                StopMoving();
            return;
        }
    }
    else
    {
        if (!me->HasInArc(pVictim, 2 * M_PI_F / 3) && !me->IsMoving())
        {
            me->SetInFront(pVictim);
            me->SendMovementPacket(MSG_MOVE_SET_FACING, false);
        }

        if (!me->HasUnitState(UNIT_STATE_MELEE_ATTACKING) &&
           (m_role != ROLE_HEALER) &&
            IsValidHostileTarget(pVictim) &&
            AttackStart(pVictim))
            return;
    }

    if (me->IsInCombat())
        UpdateInCombatAI();
}