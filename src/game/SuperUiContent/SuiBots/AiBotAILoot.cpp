/*
 * AiBotAILoot.cpp — looting, item scoring, and auto-equip for the AI bot.
 *
 * Split from the monolithic AiBotAI.cpp. THIS TU holds the gear/loot domain:
 *   - ChooseQuestReward (best quest reward by equip-score, else vendor value)
 *   - the item-scoring core: AiBotStatWeights / GetClassWeights / EquipSlotForInvType
 *     (file-local statics) + ScoreItem
 *   - DoAutoLoot, TryAutoEquipBags, TryAutoEquip
 *
 * ChooseQuestReward and ScoreItem live here (not in Combat) because they call the
 * file-local statics above — statics can't link across TUs. ScoreItem is a member,
 * so its other callers (the loot-roll path in AiBotAIMain.cpp) reach it cross-TU.
 * Cross-TU members called from here (StopMoving via none; BridgeSendEvent) resolve
 * against the sibling TUs at link time.
 */

#include "AiBotAIMain.h"
#include "AiBotCircuit.h"   // [CIRCUIT] probe macros (CIRCUIT_BOARD.md)
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

static uint8 EquipSlotForInvType(uint8 invType);

// Pick the best of a quest's CHOICE rewards (the "pick one" items). Returns the index into
// RewChoiceItemId[]. Fixed rewards (RewItemId[]) are always granted and ignore this index.
// Strategy: among usable gear choices, take the biggest equip-score GAIN over what's worn in
// that slot; if nothing is an upgrade, take the highest vendor value so the bot at least walks
// away with the most sellable item (or a usable consumable).
uint32 AiBotAI::ChooseQuestReward(Quest const* pQuest) const
{
    uint32 const count = pQuest->GetRewChoiceItemsCount();
    if (count <= 1)
    {   // cb:fold probed on next line
        CB_HITV(me->GetGUIDLow(), "cpp-loot: reward choice trivial", count);
        return 0;   // 0 or 1 choice → index 0 is the only option
    }

    int    bestUpgradeIdx = -1;  float  bestGain  = 0.0f;
    int    bestValueIdx   = 0;   uint32 bestValue = 0;

    for (uint32 i = 0; i < count; ++i)
    {
        uint32 itemId = pQuest->RewChoiceItemId[i];
        if (!itemId) continue; // cb:fold reward slot filter, choice probed at return

        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
        if (!proto) continue; // cb:fold reward slot filter, choice probed at return

        // vendor-value fallback (sell price × stack)
        uint32 stack = pQuest->RewChoiceItemCount[i] ? pQuest->RewChoiceItemCount[i] : 1;
        uint32 value = proto->SellPrice * stack;
        if (value > bestValue) { bestValue = value; bestValueIdx = (int)i; } // cb:fold best-value tracker, choice probed at return

        // equip-upgrade evaluation (gear we can actually use only)
        if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
            continue; // cb:fold reward gear filter, choice probed at return
        if (me->CanUseItem(proto) != EQUIP_ERR_OK)   // class/race/level/proficiency — §verify name
            continue; // cb:fold reward gear filter, choice probed at return

        uint8 eqSlot = EquipSlotForInvType(proto->InventoryType);
        if (eqSlot == 0xFF) continue; // cb:fold reward gear filter, choice probed at return

        float newScore = ScoreItem(proto, eqSlot);
        float oldScore = 0.0f;
        if (Item* worn = me->GetItemByPos(INVENTORY_SLOT_BAG_0, eqSlot)) // cb:fold worn-item lookup, gain probed at return
            oldScore = ScoreItem(worn->GetProto(), eqSlot); // cb:fold worn score feeds gain, choice probed at return

        float gain = newScore - oldScore;
        if (gain > bestGain) { bestGain = gain; bestUpgradeIdx = (int)i; } // cb:fold best-gain tracker, choice probed at return
    }

    if (bestUpgradeIdx >= 0)
    {   // cb:fold probed on next line
        CB_HITV(me->GetGUIDLow(), "cpp-loot: reward upgrade chosen", bestUpgradeIdx);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-REWARD] %s: quest %u — choice %d is best UPGRADE (gain=%.1f)",
            me->GetName(), pQuest->GetQuestId(), bestUpgradeIdx, bestGain);
        return (uint32)bestUpgradeIdx;
    }

    CB_HITV(me->GetGUIDLow(), "cpp-loot: reward vendor value chosen", bestValueIdx);
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-REWARD] %s: quest %u — no equip upgrade, taking highest vendor value (choice %d, %uc)",
        me->GetName(), pQuest->GetQuestId(), bestValueIdx, bestValue);
    return (uint32)bestValueIdx;
}

// ============================================================
// QuestRequiredCountFor — how many of an item the bot's active quests still call for.
// Caps looting (below) and vendors surplus (AiBotAIBridge sell path). Counts INCOMPLETE and
// COMPLETE quests — a complete quest can still be handed in, so its items are not surplus yet.
// Source items are not ReqItems, so they return 0 and are never cut.
// ============================================================
uint32 AiBotAI::QuestRequiredCountFor(uint32 itemId) const
{
    if (!me || itemId == 0)
    {   // cb:fold probed on next line
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-loot: quest-need skipped, no context");
        return 0;
    }
    uint32 maxNeeded = 0;
    const auto& questMap = me->GetQuestStatusMap();
    for (const auto& pair : questMap)
    {
        if (pair.second.m_status != QUEST_STATUS_INCOMPLETE &&
            pair.second.m_status != QUEST_STATUS_COMPLETE)
            continue; // cb:fold quest tally filter, need is the outcome
        Quest const* q = sObjectMgr.GetQuestTemplate(pair.first);
        if (!q)
            continue; // cb:fold quest tally filter, need is the outcome
        for (int j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
            if (q->ReqItemId[j] == itemId && q->ReqItemCount[j] > maxNeeded)
                maxNeeded = q->ReqItemCount[j]; // cb:fold need tally, outcome probed at caller
    }
    return maxNeeded;
}

// ============================================================
// AUTO-LOOT — walk to corpse, generate loot, take gold + items + equip + selling + bags
// ============================================================

void AiBotAI::DoAutoLoot(ObjectGuid creatureGuid)
{
    if (!me || !me->IsAlive() || !me->IsInWorld())
    {   // cb:fold probed on next line
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-loot: loot skipped, bot not ready");
        return;
    }

    Creature* creature = me->GetMap()->GetCreature(creatureGuid);
    if (!creature || creature->IsAlive())
    {   // cb:fold probed on next line
        CB_HIT(me->GetGUIDLow(), "cpp-loot: loot skipped, corpse missing or alive");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-LOOT] %s: creature not found or still alive (guid=%u), skipping",
            me->GetName(), creatureGuid.GetCounter());
        return;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-LOOT] %s: === BEGIN looting %s (entry=%u guid=%u) ===",
        me->GetName(), creature->GetName(), creature->GetEntry(), creature->GetGUIDLow());

    // --- Generate loot (same path as Player::SendLoot HIGHGUID_UNIT/LOOT_CORPSE) ---
    if (!creature->lootForBody)
    {   // cb:fold probed on next line
        CB_HIT(me->GetGUIDLow(), "cpp-loot: generating loot for body");
        if (!creature->GetOriginalLootRecipient())
        {   // cb:fold probed on next line
            CB_HIT(me->GetGUIDLow(), "cpp-loot: no loot recipient, forcing self");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-LOOT] %s: WARNING no loot recipient set — forcing to self",
                me->GetName());
            creature->SetLootRecipient(me);
        }

        creature->GenerateLootForBody(me, me->GetGroup());
        creature->lootForBody = true;

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-LOOT] %s: GenerateLootForBody → %zu items, %u copper",
            me->GetName(), creature->loot.items.size(), creature->loot.gold);

        // Log each item generated
        for (size_t i = 0; i < creature->loot.items.size(); ++i)
        {
            LootItem& li = creature->loot.items[i];
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-LOOT] %s:   slot %zu: itemId=%u count=%u looted=%d",
                me->GetName(), i, li.itemid, li.count, li.is_looted ? 1 : 0);
        }

        // Handle group loot methods if in a group
        if (Group* group = creature->GetGroupLootRecipient()) // cb:fold group deref, handling probed inside
        {   // cb:fold probed on next line
            CB_HIT(me->GetGUIDLow(), "cpp-loot: group loot recipient present");
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-LOOT] %s: group detected, loot method=%d",
                me->GetName(), (int)group->GetLootMethod());
            switch (group->GetLootMethod())
            {
                case GROUP_LOOT: // cb:fold probed on next line
                    CB_HIT(me->GetGUIDLow(), "cpp-loot: group loot roll");
                    group->GroupLoot(creature, &creature->loot);
                    break;
                case NEED_BEFORE_GREED: // cb:fold probed on next line
                    CB_HIT(me->GetGUIDLow(), "cpp-loot: need before greed roll");
                    group->NeedBeforeGreed(creature, &creature->loot);
                    break;
                default: // cb:fold probed on next line
                    CB_HIT(me->GetGUIDLow(), "cpp-loot: loot method plain take");
                    break;
            }
        }
    }
    else
    {   // cb:fold probed on next line
        CB_HIT(me->GetGUIDLow(), "cpp-loot: loot already generated");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-LOOT] %s: lootForBody already true — %zu items, %u copper (pre-generated)",
            me->GetName(), creature->loot.items.size(), creature->loot.gold);
    }

    // --- Take money ---
    uint32 gold = creature->loot.gold;
    if (gold > 0)
    {   // cb:fold probed on next line
        CB_HITV(me->GetGUIDLow(), "cpp-loot: gold on corpse", gold);
        uint32 moneyBefore = me->GetMoney();
        if (Group* group = me->GetGroup()) // cb:fold group deref, split probed inside
        {   // cb:fold probed on next line
            CB_HIT(me->GetGUIDLow(), "cpp-loot: gold split with group");
            std::vector<Player*> playersNear;
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->getSource();
                if (member && me->IsWithinLootXPDist(member))
                    playersNear.push_back(member); // cb:fold nearby member tally for the split
            }
            if (playersNear.empty())
            {
                CB_HITV(me->GetGUIDLow(), "cpp-loot: empty group split, falling back to self", gold);
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-LOOT] %s: WARNING group gold split found no nearby members; "
                    "crediting %u copper to the looter",
                    me->GetName(), gold);
                playersNear.push_back(me);
            }
            uint32 share = gold / (uint32)playersNear.size();
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-LOOT] %s: splitting %u copper among %zu members (%u each)",
                me->GetName(), gold, playersNear.size(), share);
            for (Player* member : playersNear)
            {
                member->ModifyMoney((int32)share);
                member->LootMoney((int32)share, &creature->loot);
            }
        }
        else
        {   // cb:fold probed on next line
            CB_HIT(me->GetGUIDLow(), "cpp-loot: gold taken solo");
            me->ModifyMoney((int32)gold);
            me->LootMoney((int32)gold, &creature->loot);
        }
        creature->loot.gold = 0;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-LOOT] %s: gold %u → %u (+%u copper)",
            me->GetName(), moneyBefore, me->GetMoney(), gold);
    }
    else
    {   // cb:fold probed on next line
        CB_HIT(me->GetGUIDLow(), "cpp-loot: no gold on corpse");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-LOOT] %s: no gold on corpse", me->GetName());
    }

    // --- Take items ---
    me->AutoStoreLoot(creature->loot);

    // --- Cap quest-gather items at the requirement (the boar-ribs over-loot fix) ---
    // A normal white/food item a quest wants N of keeps dropping as ordinary loot; without a
    // cap the bot hoards dozens it cannot vendor (the sell path protects quest items). Trim each
    // stored quest item back to what the active quests still need. DestroyItemCount is the same
    // stock primitive the sell path uses, so we don't reimplement AutoStoreLoot's placement.
    // Runs every loot, so it also self-heals dozens already hoarded — the next kill trims them
    // with no vendor trip. Destroying surplus above the requirement never lowers quest credit
    // (progress = min(have, req); have stays >= req). A repeated itemId across loot slots is a
    // cheap no-op on later passes (have <= need once trimmed), so no dedup is needed.
    for (size_t i = 0; i < creature->loot.items.size(); ++i)
    {
        uint32 itemId = creature->loot.items[i].itemid;
        if (itemId == 0)
            continue; // cb:fold quest-cap filter, trim probed below
        uint32 need = QuestRequiredCountFor(itemId);
        if (need == 0)
            continue; // cb:fold quest-cap filter, not a quest item
        uint32 have = me->GetItemCount(itemId, false);
        if (have <= need)
            continue; // cb:fold quest-cap filter, within requirement
        uint32 surplus = have - need;
        CB_HITV(me->GetGUIDLow(), "cpp-loot: quest-cap trimming surplus", itemId);
        me->DestroyItemCount(itemId, surplus, true);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-LOOT] %s: quest-cap itemId=%u kept %u, dropped %u surplus",
            me->GetName(), itemId, need, surplus);
    }

    // --- Build loot summary for bridge event ---
    std::string lootData = "gold=" + std::to_string(gold);
    std::string itemStr;
    uint32 itemsLooted = 0;
    for (size_t i = 0; i < creature->loot.items.size(); ++i)
    {
        LootItem& item = creature->loot.items[i];
        if (item.is_looted)
        {   // cb:fold loot summary assembly, LOOT event carries the payload
            itemsLooted++;
            if (!itemStr.empty()) itemStr += ","; // cb:fold summary assembly, LOOT event carries the payload
            itemStr += std::to_string(item.itemid) + ":" + std::to_string(item.count);
        }
    }
    if (!itemStr.empty())
        lootData += "|items=" + itemStr; // cb:fold summary assembly, LOOT event carries the payload

    BridgeSendEvent("LOOT", lootData.c_str());
    // --- Auto-equip bags first (more capacity), then gear ---
    TryAutoEquipBags();
    TryAutoEquip();

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-LOOT] %s: === DONE === %u items stored, %u copper | bridge: %s",
        me->GetName(), itemsLooted, gold, lootData.c_str());

    // --- Cleanup: mark fully looted, accelerate corpse decay ---
    creature->loot.clear();
    creature->RemoveFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
    creature->AllLootRemovedFromCorpse();
}


void AiBotAI::TryAutoEquipBags()
{
    if (!me || !me->IsAlive() || !me->IsInWorld())
    {   // cb:fold probed on next line
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-loot: bag scan skipped, bot not ready");
        return;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-BAGS] %s: scanning for bag upgrades...", me->GetName());

    bool changed = false;
    std::string bagEvents;

    for (int pass = 0; pass < 8; ++pass)
    {
        bool foundAction = false;

        // --- Collect candidate bags from all inventory ---
        struct BagCandidate { Item* item; uint8 bag; uint8 slot; uint32 size; };
        std::vector<BagCandidate> candidates;

        // Backpack
        for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            Item* pItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)i);
            if (!pItem) continue; // cb:fold bag scan filter, actions probed below
            ItemPrototype const* proto = pItem->GetProto();
            if (!proto || proto->Class != ITEM_CLASS_CONTAINER ||
                proto->SubClass != ITEM_SUBCLASS_CONTAINER || proto->ContainerSlots == 0)
                continue; // cb:fold bag scan filter, actions probed below
            candidates.push_back({pItem, INVENTORY_SLOT_BAG_0, (uint8)i, proto->ContainerSlots});
        }

        // Inside extra bags
        for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        {
            Bag* pEquippedBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b);
            if (!pEquippedBag || pEquippedBag->GetProto()->Class != ITEM_CLASS_CONTAINER)
                continue; // cb:fold bag scan filter, actions probed below
            for (uint32 j = 0; j < pEquippedBag->GetBagSize(); ++j)
            {
                Item* pItem = me->GetItemByPos((uint8)b, (uint8)j);
                if (!pItem) continue; // cb:fold bag scan filter, actions probed below
                ItemPrototype const* proto = pItem->GetProto();
                if (!proto || proto->Class != ITEM_CLASS_CONTAINER ||
                    proto->SubClass != ITEM_SUBCLASS_CONTAINER || proto->ContainerSlots == 0)
                    continue; // cb:fold bag scan filter, actions probed below
                candidates.push_back({pItem, (uint8)b, (uint8)j, proto->ContainerSlots});
            }
        }

        if (candidates.empty()) break; // cb:fold no candidate bags ends the pass loop

        // Sort biggest-first
        std::sort(candidates.begin(), candidates.end(),
            [](const BagCandidate& a, const BagCandidate& b) { return a.size > b.size; });

        for (auto& cand : candidates)
        {
            if (foundAction) break; // cb:fold restart latch, action probed where taken

            // --- Priority 1: Empty bag slot ---
            for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            {
                if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b))
                {   // cb:fold probed on next line
                    CB_HITV(me->GetGUIDLow(), "cpp-loot: bag equipped to empty slot", cand.size);
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-BAGS] %s: equipping [%s] (%u slots) → empty bag slot %d",
                        me->GetName(),
                        cand.item->GetProto()->Name1 ? cand.item->GetProto()->Name1 : "?",
                        cand.size, b);

                    uint16 dest = (INVENTORY_SLOT_BAG_0 << 8) | (uint8)b;
                    me->RemoveItem(cand.bag, cand.slot, false);
                    cand.item->RemoveFromUpdateQueueOf(me);
                    me->EquipItem(dest, cand.item, true);

                    if (!bagEvents.empty()) bagEvents += ","; // cb:fold event string assembly, BAG_EQUIP carries it
                    bagEvents += std::to_string(cand.item->GetEntry()) + ":" + std::to_string(b);

                    changed = true;
                    foundAction = true;
                    break;
                }
            }
            if (foundAction) break; // cb:fold restart latch, action probed where taken

            // --- Priority 2: Replace smallest EMPTY equipped bag ---
            int worstSlot = -1;
            uint32 worstSize = 999;

            for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            {
                Bag* pBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b);
                if (!pBag) continue; // cb:fold bag replace filter, upgrade probed below
                uint32 equippedSize = pBag->GetBagSize();
                if (equippedSize >= cand.size) continue; // cb:fold bag replace filter, upgrade probed below
                if (!pBag->IsEmpty()) continue; // cb:fold bag replace filter, upgrade probed below

                if (equippedSize < worstSize)
                {   // cb:fold smallest-bag tracker, upgrade probed below
                    worstSize = equippedSize;
                    worstSlot = b;
                }
            }

            if (worstSlot >= 0)
            {   // cb:fold probed on next line
                CB_HITV(me->GetGUIDLow(), "cpp-loot: bag upgrade replacing slot", worstSlot);
                Bag* pOldBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)worstSlot);

                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-BAGS] %s: UPGRADE bag slot %d: [%s] (%u) → [%s] (%u slots)",
                    me->GetName(), worstSlot,
                    pOldBag->GetProto()->Name1 ? pOldBag->GetProto()->Name1 : "?", worstSize,
                    cand.item->GetProto()->Name1 ? cand.item->GetProto()->Name1 : "?", cand.size);

                uint16 dest = (INVENTORY_SLOT_BAG_0 << 8) | (uint8)worstSlot;

                // Remove candidate from inventory FIRST (it might be inside the old bag)
                me->RemoveItem(cand.bag, cand.slot, false);
                cand.item->RemoveFromUpdateQueueOf(me);

                // Now remove old bag from equip slot
                me->RemoveItem(INVENTORY_SLOT_BAG_0, (uint8)worstSlot, false);
                pOldBag->RemoveFromUpdateQueueOf(me);

                // Equip new bag
                me->EquipItem(dest, cand.item, true);

                // Store old bag in inventory
                ItemPosCountVec sDest;
                InventoryResult storeRes = me->CanStoreItem(NULL_BAG, NULL_SLOT, sDest, (Item*)pOldBag, false);
                if (storeRes == EQUIP_ERR_OK)
                    me->StoreItem(sDest, (Item*)pOldBag, true); // cb:fold old bag stored ok, failure arm probed
                else
                {   // cb:fold probed on next line
                    CB_HIT(me->GetGUIDLow(), "cpp-loot: old bag destroyed, no room");
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-BAGS] %s: WARNING could not store old bag %u, destroying",
                        me->GetName(), pOldBag->GetEntry());
                    pOldBag->RemoveFromUpdateQueueOf(me);
                    pOldBag->SetState(ITEM_REMOVED, me);
                }

                if (!bagEvents.empty()) bagEvents += ","; // cb:fold event string assembly, BAG_EQUIP carries it
                bagEvents += std::to_string(cand.item->GetEntry()) + ":" + std::to_string(worstSlot);

                changed = true;
                foundAction = true;
            }
        }

        if (!foundAction) break; // cb:fold passes settled, event probed below
    }

    if (changed)
    {   // cb:fold probed on next line
        CB_HIT(me->GetGUIDLow(), "cpp-loot: bag equip event sent");
        // Calculate total slots for the event
        uint32 totalSlots = (INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START);
        for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        {
            Bag* p = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)i);
            if (p && p->GetProto()->Class == ITEM_CLASS_CONTAINER &&
                p->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER)
                totalSlots += p->GetBagSize(); // cb:fold slot tally, event carries the count
        }

        uint32 freeSlots = 0;
        for (int fi = INVENTORY_SLOT_ITEM_START; fi < INVENTORY_SLOT_ITEM_END; ++fi)
            if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, fi))
                ++freeSlots; // cb:fold free-slot tally, event carries the count
        for (int fi = INVENTORY_SLOT_BAG_START; fi < INVENTORY_SLOT_BAG_END; ++fi)
            if (Bag* pFBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, fi)) // cb:fold free-slot tally, event carries the count
                if (pFBag->GetProto()->Class == ITEM_CLASS_CONTAINER && pFBag->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER) // cb:fold free-slot tally, event carries the count
                    for (uint32 fj = 0; fj < pFBag->GetBagSize(); ++fj) // cb:fold free-slot tally, event carries the count
                        if (!me->GetItemByPos(fi, fj))
                            ++freeSlots; // cb:fold free-slot tally, event carries the count
        char eventData[256];
        snprintf(eventData, sizeof(eventData), "bags=%s|free_slots=%u|total_slots=%u",
            bagEvents.c_str(), freeSlots, totalSlots);
        BridgeSendEvent("BAG_EQUIP", eventData);

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BAGS] %s: === DONE === %s | %u/%u slots free",
            me->GetName(), bagEvents.c_str(), freeSlots, totalSlots);
    }
}

// Per-class scoring weights. Vanilla 1.12 realities baked in:
//  - +spell damage / +healing / +AP / mp5 are NOT in ItemStat — they live in the item's
//    ON-EQUIP spell effects (proto->Spells[]), so we parse those below. THIS is why a caster
//    green (its value = +spelldmg) used to lose to a higher-armor white.
//  - Armor is downweighted hard for cloth users (was a flat 1.0 that drowned int/spi).
//  - A quality multiplier + a real ItemLevel term make green > white in the same slot
//    almost always (higher budget) without hardcoding it.
struct AiBotStatWeights
{
    float str, agi, sta, intel, spi;     // ItemStat primaries
    float armor;                         // per-point armor
    float spellDmg, healing, mp5, ap;    // parsed ON-EQUIP spell effects
    float weaponDps;                     // melee/ranged white-damage value
};

static AiBotStatWeights GetClassWeights(uint8 cls)
{
    switch (cls)
    {                          // str   agi   sta   int   spi   armor  sdmg  heal  mp5   ap    wdps
        case CLASS_WARRIOR: return { 1.6f, 0.7f, 1.2f, 0.0f, 0.0f, 0.55f, 0.0f, 0.0f, 0.0f, 1.1f, 6.0f }; // cb:fold pure class weights, no bot context
        case CLASS_ROGUE:   return { 1.0f, 1.6f, 0.9f, 0.0f, 0.0f, 0.25f, 0.0f, 0.0f, 0.0f, 1.0f, 6.0f }; // cb:fold pure class weights, no bot context
        case CLASS_HUNTER:  return { 0.6f, 1.6f, 0.9f, 0.4f, 0.0f, 0.25f, 0.0f, 0.0f, 0.0f, 1.0f, 5.0f }; // cb:fold pure class weights, no bot context
        case CLASS_PALADIN: return { 1.4f, 0.5f, 1.2f, 0.7f, 0.6f, 0.50f, 0.6f, 0.9f, 0.7f, 1.0f, 5.0f }; // cb:fold pure class weights, no bot context
        case CLASS_SHAMAN:  return { 1.0f, 0.8f, 1.1f, 1.1f, 0.8f, 0.35f, 1.2f, 1.0f, 0.9f, 0.9f, 4.0f }; // cb:fold pure class weights, no bot context
        case CLASS_DRUID:   return { 1.0f, 0.9f, 1.1f, 1.1f, 0.8f, 0.20f, 1.2f, 1.0f, 0.9f, 0.9f, 4.0f }; // cb:fold pure class weights, no bot context
        case CLASS_PRIEST:  return { 0.0f, 0.2f, 0.7f, 1.3f, 1.0f, 0.05f, 1.6f, 1.6f, 1.2f, 0.0f, 1.0f }; // cb:fold pure class weights, no bot context
        case CLASS_MAGE:    return { 0.0f, 0.2f, 0.7f, 1.3f, 0.8f, 0.05f, 2.0f, 0.0f, 1.0f, 0.0f, 1.0f }; // cb:fold pure class weights, no bot context
        case CLASS_WARLOCK: return { 0.0f, 0.2f, 0.7f, 1.3f, 0.8f, 0.05f, 2.0f, 0.0f, 1.0f, 0.0f, 1.0f }; // cb:fold pure class weights, no bot context
        default:            return { 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.30f, 1.0f, 1.0f, 0.8f, 1.0f, 4.0f }; // cb:fold pure class weights, no bot context
    }
}

// InventoryType → equipment slot (the same mapping the loot need/greed roll does inline).
static uint8 EquipSlotForInvType(uint8 invType)
{
    switch (invType)
    {
        case INVTYPE_HEAD:            return EQUIPMENT_SLOT_HEAD; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_NECK:            return EQUIPMENT_SLOT_NECK; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_SHOULDERS:       return EQUIPMENT_SLOT_SHOULDERS; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_BODY:            return EQUIPMENT_SLOT_BODY; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_CHEST: // cb:fold pure invtype mapping, no bot context
        case INVTYPE_ROBE:            return EQUIPMENT_SLOT_CHEST; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_WAIST:           return EQUIPMENT_SLOT_WAIST; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_LEGS:            return EQUIPMENT_SLOT_LEGS; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_FEET:            return EQUIPMENT_SLOT_FEET; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_WRISTS:          return EQUIPMENT_SLOT_WRISTS; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_HANDS:           return EQUIPMENT_SLOT_HANDS; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_FINGER:          return EQUIPMENT_SLOT_FINGER1; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_TRINKET:         return EQUIPMENT_SLOT_TRINKET1; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_CLOAK:           return EQUIPMENT_SLOT_BACK; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_WEAPON: // cb:fold pure invtype mapping, no bot context
        case INVTYPE_2HWEAPON: // cb:fold pure invtype mapping, no bot context
        case INVTYPE_WEAPONMAINHAND:  return EQUIPMENT_SLOT_MAINHAND; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_SHIELD: // cb:fold pure invtype mapping, no bot context
        case INVTYPE_WEAPONOFFHAND: // cb:fold pure invtype mapping, no bot context
        case INVTYPE_HOLDABLE:        return EQUIPMENT_SLOT_OFFHAND; // cb:fold pure invtype mapping, no bot context
        case INVTYPE_RANGED: // cb:fold pure invtype mapping, no bot context
        case INVTYPE_THROWN: // cb:fold pure invtype mapping, no bot context
        case INVTYPE_RANGEDRIGHT:     return EQUIPMENT_SLOT_RANGED; // cb:fold pure invtype mapping, no bot context
        default:                      return 0xFF; // cb:fold pure invtype mapping, no bot context
    }
}

float AiBotAI::ScoreItem(ItemPrototype const* proto, uint8 slot) const
{
    if (!proto)
        return 0.0f; // cb:fold null proto scores zero, score probed at return

    // Active Warrior/Paladin tanks require a one-handed weapon and shield.
    // Apply the same rejection to candidates and currently equipped items so
    // an incompatible 2H/off-hand scores low enough for the next valid item to
    // replace it.  Feral tanks deliberately retain their normal scoring.
    bool const shieldTank = GetCombatActiveRole() == ROLE_TANK &&
        (me->GetClass() == CLASS_WARRIOR || me->GetClass() == CLASS_PALADIN);
    if (shieldTank)
    {   // cb:fold tank weapon gate, rejections probed inside
        if (slot == EQUIPMENT_SLOT_MAINHAND &&
            (!proto->IsWeapon() || proto->InventoryType == INVTYPE_2HWEAPON))
        {   // cb:fold probed on next line
            CB_HITV(me->GetGUIDLow(), "cpp-loot: score reject, tank needs 1h weapon", proto->ItemId);
            return -100000.0f;
        }
        if (slot == EQUIPMENT_SLOT_OFFHAND && proto->InventoryType != INVTYPE_SHIELD)
        {   // cb:fold probed on next line
            CB_HITV(me->GetGUIDLow(), "cpp-loot: score reject, tank needs shield", proto->ItemId);
            return -100000.0f;
        }
    }
    // Non-tank Arms remains deliberately two-handed-axe specialized.
    else if (me->GetClass() == CLASS_WARRIOR && GetCombatSpecTab() == 0)
    {   // cb:fold arms axe gate, rejections probed inside
        if (slot == EQUIPMENT_SLOT_MAINHAND &&
           (!proto->IsWeapon() || proto->SubClass != ITEM_SUBCLASS_WEAPON_AXE2))
        {   // cb:fold probed on next line
            CB_HITV(me->GetGUIDLow(), "cpp-loot: score reject, arms wants 2h axe", proto->ItemId);
            return -100000.0f;
        }
        if (slot == EQUIPMENT_SLOT_OFFHAND)
        {   // cb:fold probed on next line
            CB_HITV(me->GetGUIDLow(), "cpp-loot: score reject, arms offhand blocked", proto->ItemId);
            return -100000.0f;
        }
    }

    AiBotStatWeights const w = GetClassWeights(me->GetClass());
    float score = 0.0f;

    // 1) Primary stats (ItemStat)
    for (auto const& s : proto->ItemStat)
    {
        if (s.ItemStatValue == 0) continue; // cb:fold stat accumulation, score probed at return
        float v = (float)s.ItemStatValue;
        switch (s.ItemStatType)
        {
            case ITEM_MOD_STRENGTH:  score += v * w.str;   break; // cb:fold stat accumulation, score probed at return
            case ITEM_MOD_AGILITY:   score += v * w.agi;   break; // cb:fold stat accumulation, score probed at return
            case ITEM_MOD_STAMINA:   score += v * w.sta;   break; // cb:fold stat accumulation, score probed at return
            case ITEM_MOD_INTELLECT: score += v * w.intel; break; // cb:fold stat accumulation, score probed at return
            case ITEM_MOD_SPIRIT:    score += v * w.spi;   break; // cb:fold stat accumulation, score probed at return
            default:                 score += v * 0.3f;    break;   // misc, small credit // cb:fold stat accumulation, score probed at return
        }
    }

    // 2) ON-EQUIP spell effects — the vanilla "hidden" budget (spell dmg / healing / AP / mp5).
    //    Walk the item's equip-triggered spells and credit their auras. (§verify the field names.)
    const uint32 ON_EQUIP = 1;   // ITEM_SPELLTRIGGER_ON_EQUIP
    for (int si = 0; si < MAX_ITEM_PROTO_SPELLS; ++si)
    {
        if (proto->Spells[si].SpellTrigger != ON_EQUIP) continue; // cb:fold equip-spell accumulation, score probed at return
        int32 spellId = proto->Spells[si].SpellId;
        if (spellId <= 0) continue; // cb:fold equip-spell accumulation, score probed at return

        SpellEntry const* se = sSpellMgr.GetSpellEntry((uint32)spellId);
        if (!se) continue; // cb:fold equip-spell accumulation, score probed at return

        for (int e = 0; e < 3; ++e)
        {
            if (se->Effect[e] != SPELL_EFFECT_APPLY_AURA) continue; // cb:fold equip-spell accumulation, score probed at return
            float val = (float)(se->EffectBasePoints[e] + 1);   // vanilla stores value-1
            if (val <= 0.0f) continue; // cb:fold equip-spell accumulation, score probed at return

            switch (se->EffectApplyAuraName[e])
            {
                case SPELL_AURA_MOD_DAMAGE_DONE:         score += val * w.spellDmg;   break; // +spell dmg // cb:fold aura accumulation, score probed at return
                case SPELL_AURA_MOD_HEALING_DONE:        score += val * w.healing;    break; // cb:fold aura accumulation, score probed at return
                case SPELL_AURA_MOD_POWER_REGEN:         score += val * w.mp5;        break; // cb:fold aura accumulation, score probed at return
                case SPELL_AURA_MOD_ATTACK_POWER:        score += val * w.ap;         break; // cb:fold aura accumulation, score probed at return
                case SPELL_AURA_MOD_RANGED_ATTACK_POWER: score += val * w.ap;         break; // cb:fold aura accumulation, score probed at return
                case SPELL_AURA_MOD_STAT:                score += val * 0.8f;         break; // +stat // cb:fold aura accumulation, score probed at return
                case SPELL_AURA_MOD_INCREASE_HEALTH:     score += val * w.sta * 0.1f; break; // cb:fold aura accumulation, score probed at return
                case SPELL_AURA_MOD_RESISTANCE:          score += val * 0.5f;         break; // cb:fold aura accumulation, score probed at return
                default: break; // cb:fold aura accumulation, score probed at return
            }
        }
    }

    // 3) Armor — class-scaled, downweighted
    if (proto->Armor > 0)
        score += (float)proto->Armor * w.armor; // cb:fold armor accumulation, score probed at return

    // 4) Shield block
    if (proto->Block > 0)
        score += (float)proto->Block * 0.5f; // cb:fold block accumulation, score probed at return

    // 5) Weapon white DPS
    if (proto->IsWeapon() && proto->Delay > 0)
    {   // cb:fold weapon dps accumulation, score probed at return
        float dmg = 0.0f;
        for (int i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i)
        {
            if (proto->Damage[i].DamageMax == 0.0f) break; // cb:fold damage table end, score probed at return
            dmg += (proto->Damage[i].DamageMin + proto->Damage[i].DamageMax) / 2.0f;
        }
        float dps = dmg * 1000.0f / (float)proto->Delay;
        score += dps * w.weaponDps;
    }

    // 6) ItemLevel as a budget proxy — moderate (captures hidden budget the parse misses)
    score += (float)proto->ItemLevel * 0.6f;

    // 7) Quality multiplier — the "green > white" thumb. Applied last so it scales the WHOLE
    //    score: comparable items break toward higher quality, but a vastly better white still
    //    wins (we never take a level-1 green over a level-12 white in the same slot).
    float qmul;
    switch (proto->Quality)
    {
        case ITEM_QUALITY_POOR:     qmul = 0.85f; break; // grey // cb:fold quality table, score probed at return
        case ITEM_QUALITY_NORMAL:   qmul = 1.00f; break; // white // cb:fold quality table, score probed at return
        case ITEM_QUALITY_UNCOMMON: qmul = 1.15f; break; // green // cb:fold quality table, score probed at return
        case ITEM_QUALITY_RARE:     qmul = 1.30f; break; // blue // cb:fold quality table, score probed at return
        case ITEM_QUALITY_EPIC:     qmul = 1.45f; break; // purple // cb:fold quality table, score probed at return
        default:                    qmul = 1.50f; break; // legendary+ // cb:fold quality table, score probed at return
    }
    score *= qmul;

    CB_HITV(me->GetGUIDLow(), "cpp-loot: item scored", score);
    return score;
}

void AiBotAI::TryAutoEquip()
{
    if (!me || !me->IsAlive() || !me->IsInWorld() || me->IsInCombat())
    {   // cb:fold probed on next line
        CB_HIT(me ? me->GetGUIDLow() : 0, "cpp-loot: equip scan skipped, busy or gone");
        return;
    }
 
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-EQUIP] %s: scanning bags for gear upgrades...", me->GetName());
 
    bool equipped = false;
    std::string equipEvents;
 
    for (int pass = 0; pass < 20; ++pass)
    {
        bool foundUpgrade = false;
 
        // Helper lambda: check one bag slot for equippable upgrades
        auto checkItem = [&](uint8 bag, uint8 slot) -> bool
        {
            Item* pItem = me->GetItemByPos(bag, slot);
            if (!pItem) return false; // cb:fold equip scan filter, upgrades probed below

            ItemPrototype const* proto = pItem->GetProto();
            if (!proto) return false; // cb:fold equip scan filter, upgrades probed below

            // Only weapons and armor (bags handled by TryAutoEquipBags)
            if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
                return false; // cb:fold equip scan filter, upgrades probed below

            uint16 dest = 0;
            InventoryResult canEquip = me->CanEquipItem(NULL_SLOT, dest, pItem, true);
            if (canEquip != EQUIP_ERR_OK)
                return false; // cb:fold equip scan filter, upgrades probed below
 
            uint8 targetSlot = dest & 0xFF;
 
            float newScore = ScoreItem(proto, targetSlot);
 
            float oldScore = 0.0f;
            Item* pOldItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, targetSlot);
            if (pOldItem)
                oldScore = ScoreItem(pOldItem->GetProto(), targetSlot); // cb:fold worn score feeds compare, upgrades probed below
 
            if (newScore <= oldScore)
                return false; // cb:fold not an upgrade, upgrades probed below
 
            // --- Upgrade! ---
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-EQUIP] %s: UPGRADE slot %u: [%s] (%.1f) → [%s] (%.1f)",
                me->GetName(), targetSlot,
                pOldItem && pOldItem->GetProto() ? pOldItem->GetProto()->Name1 : "(empty)",
                oldScore,
                proto->Name1 ? proto->Name1 : "?",
                newScore);
 
            uint8 srcBag = pItem->GetBagSlot();
            uint8 srcSlot = pItem->GetSlot();
 
            if (!pOldItem)
            {   // cb:fold probed on next line
                CB_HITV(me->GetGUIDLow(), "cpp-loot: equip into empty slot", proto->ItemId);
                // No existing item in target slot — just move new item there
                me->RemoveItem(srcBag, srcSlot, false);
                pItem->RemoveFromUpdateQueueOf(me);
                me->EquipItem(dest, pItem, true);
            }
            else
            {   // cb:fold probed on next line
                CB_HITV(me->GetGUIDLow(), "cpp-loot: equip swap with worn item", proto->ItemId);
                // Swap: unequip old, equip new, store old in bags
                uint8 dstBag = pOldItem->GetBagSlot();
                uint8 dstSlot = pOldItem->GetSlot();
 
                me->RemoveItem(dstBag, dstSlot, false);
                pOldItem->RemoveFromUpdateQueueOf(me);
                me->RemoveItem(srcBag, srcSlot, false);
                pItem->RemoveFromUpdateQueueOf(me);
 
                me->EquipItem(dest, pItem, true);
 
                ItemPosCountVec sDest;
                InventoryResult storeRes = me->CanStoreItem(NULL_BAG, NULL_SLOT, sDest, pOldItem, false);
                if (storeRes == EQUIP_ERR_OK)
                    me->StoreItem(sDest, pOldItem, true); // cb:fold old item stored ok, failure arms probed
                else
                {   // cb:fold probed on next line
                    CB_HIT(me->GetGUIDLow(), "cpp-loot: old item fallback store attempt");
                    // Try the slot the new item came from
                    sDest.clear();
                    storeRes = me->CanStoreItem(srcBag, srcSlot, sDest, pOldItem, false);
                    if (storeRes == EQUIP_ERR_OK)
                        me->StoreItem(sDest, pOldItem, true); // cb:fold old item stored ok on fallback, destroy probed below
                    else
                    {   // cb:fold probed on next line
                        CB_HIT(me->GetGUIDLow(), "cpp-loot: old item destroyed, no room");
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT-EQUIP] %s: WARNING could not store old item %u, destroying",
                            me->GetName(), pOldItem->GetEntry());
                        pOldItem->RemoveFromUpdateQueueOf(me);
                        pOldItem->SetState(ITEM_REMOVED, me);
                    }
                }
            }
 
            me->AutoUnequipOffhandIfNeed();
 
            if (!equipEvents.empty()) equipEvents += ","; // cb:fold event string assembly, EQUIP carries it
            equipEvents += std::to_string(proto->ItemId) + ":" + std::to_string(targetSlot);
 
            equipped = true;
            return true; // signal restart
        };
 
        // Scan backpack
        for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END && !foundUpgrade; ++i)
            foundUpgrade = checkItem(INVENTORY_SLOT_BAG_0, (uint8)i);
 
        // Scan extra bags
        if (!foundUpgrade)
        {   // cb:fold scan extra bags when backpack dry, upgrades probed in checkItem
            for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END && !foundUpgrade; ++b)
            {
                Bag* pBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b);
                if (!pBag || pBag->GetProto()->Class != ITEM_CLASS_CONTAINER)
                    continue; // cb:fold equip scan filter, upgrades probed above
                for (uint32 j = 0; j < pBag->GetBagSize() && !foundUpgrade; ++j)
                    foundUpgrade = checkItem((uint8)b, (uint8)j);
            }
        }
 
        if (!foundUpgrade) break; // cb:fold passes settled, event probed below
    }

    if (equipped)
    {   // cb:fold probed on next line
        CB_HIT(me->GetGUIDLow(), "cpp-loot: equip event sent");
        char eventData[256];
        snprintf(eventData, sizeof(eventData), "items=%s", equipEvents.c_str());
        BridgeSendEvent("EQUIP", eventData);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-EQUIP] %s: === DONE === equipped: %s", me->GetName(), equipEvents.c_str());
    }
}
