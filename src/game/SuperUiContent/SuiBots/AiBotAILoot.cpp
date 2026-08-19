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
        return 0;   // 0 or 1 choice → index 0 is the only option

    int    bestUpgradeIdx = -1;  float  bestGain  = 0.0f;
    int    bestValueIdx   = 0;   uint32 bestValue = 0;

    for (uint32 i = 0; i < count; ++i)
    {
        uint32 itemId = pQuest->RewChoiceItemId[i];
        if (!itemId) continue;

        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
        if (!proto) continue;

        // vendor-value fallback (sell price × stack)
        uint32 stack = pQuest->RewChoiceItemCount[i] ? pQuest->RewChoiceItemCount[i] : 1;
        uint32 value = proto->SellPrice * stack;
        if (value > bestValue) { bestValue = value; bestValueIdx = (int)i; }

        // equip-upgrade evaluation (gear we can actually use only)
        if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
            continue;
        if (me->CanUseItem(proto) != EQUIP_ERR_OK)   // class/race/level/proficiency — §verify name
            continue;

        uint8 eqSlot = EquipSlotForInvType(proto->InventoryType);
        if (eqSlot == 0xFF) continue;

        float newScore = ScoreItem(proto, eqSlot);
        float oldScore = 0.0f;
        if (Item* worn = me->GetItemByPos(INVENTORY_SLOT_BAG_0, eqSlot))
            oldScore = ScoreItem(worn->GetProto(), eqSlot);

        float gain = newScore - oldScore;
        if (gain > bestGain) { bestGain = gain; bestUpgradeIdx = (int)i; }
    }

    if (bestUpgradeIdx >= 0)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-REWARD] %s: quest %u — choice %d is best UPGRADE (gain=%.1f)",
            me->GetName(), pQuest->GetQuestId(), bestUpgradeIdx, bestGain);
        return (uint32)bestUpgradeIdx;
    }

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
        return 0;
    uint32 maxNeeded = 0;
    const auto& questMap = me->GetQuestStatusMap();
    for (const auto& pair : questMap)
    {
        if (pair.second.m_status != QUEST_STATUS_INCOMPLETE &&
            pair.second.m_status != QUEST_STATUS_COMPLETE)
            continue;
        Quest const* q = sObjectMgr.GetQuestTemplate(pair.first);
        if (!q)
            continue;
        for (int j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
            if (q->ReqItemId[j] == itemId && q->ReqItemCount[j] > maxNeeded)
                maxNeeded = q->ReqItemCount[j];
    }
    return maxNeeded;
}

// ============================================================
// AUTO-LOOT — walk to corpse, generate loot, take gold + items + equip + selling + bags
// ============================================================

void AiBotAI::DoAutoLoot(ObjectGuid creatureGuid)
{
    if (!me || !me->IsAlive() || !me->IsInWorld())
        return;

    Creature* creature = me->GetMap()->GetCreature(creatureGuid);
    if (!creature || creature->IsAlive())
    {
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
    {
        if (!creature->GetOriginalLootRecipient())
        {
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
        if (Group* group = creature->GetGroupLootRecipient())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-LOOT] %s: group detected, loot method=%d",
                me->GetName(), (int)group->GetLootMethod());
            switch (group->GetLootMethod())
            {
                case GROUP_LOOT:
                    group->GroupLoot(creature, &creature->loot);
                    break;
                case NEED_BEFORE_GREED:
                    group->NeedBeforeGreed(creature, &creature->loot);
                    break;
                default:
                    break;
            }
        }
    }
    else
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-LOOT] %s: lootForBody already true — %zu items, %u copper (pre-generated)",
            me->GetName(), creature->loot.items.size(), creature->loot.gold);
    }

    // --- Take money ---
    uint32 gold = creature->loot.gold;
    if (gold > 0)
    {
        uint32 moneyBefore = me->GetMoney();
        if (Group* group = me->GetGroup())
        {
            std::vector<Player*> playersNear;
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->getSource();
                if (member && me->IsWithinLootXPDist(member))
                    playersNear.push_back(member);
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
        {
            me->ModifyMoney((int32)gold);
            me->LootMoney((int32)gold, &creature->loot);
        }
        creature->loot.gold = 0;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-LOOT] %s: gold %u → %u (+%u copper)",
            me->GetName(), moneyBefore, me->GetMoney(), gold);
    }
    else
    {
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
            continue;
        uint32 need = QuestRequiredCountFor(itemId);
        if (need == 0)
            continue;
        uint32 have = me->GetItemCount(itemId, false);
        if (have <= need)
            continue;
        uint32 surplus = have - need;
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
        {
            itemsLooted++;
            if (!itemStr.empty()) itemStr += ",";
            itemStr += std::to_string(item.itemid) + ":" + std::to_string(item.count);
        }
    }
    if (!itemStr.empty())
        lootData += "|items=" + itemStr;

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
        return;

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
            if (!pItem) continue;
            ItemPrototype const* proto = pItem->GetProto();
            if (!proto || proto->Class != ITEM_CLASS_CONTAINER ||
                proto->SubClass != ITEM_SUBCLASS_CONTAINER || proto->ContainerSlots == 0)
                continue;
            candidates.push_back({pItem, INVENTORY_SLOT_BAG_0, (uint8)i, proto->ContainerSlots});
        }

        // Inside extra bags
        for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        {
            Bag* pEquippedBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b);
            if (!pEquippedBag || pEquippedBag->GetProto()->Class != ITEM_CLASS_CONTAINER)
                continue;
            for (uint32 j = 0; j < pEquippedBag->GetBagSize(); ++j)
            {
                Item* pItem = me->GetItemByPos((uint8)b, (uint8)j);
                if (!pItem) continue;
                ItemPrototype const* proto = pItem->GetProto();
                if (!proto || proto->Class != ITEM_CLASS_CONTAINER ||
                    proto->SubClass != ITEM_SUBCLASS_CONTAINER || proto->ContainerSlots == 0)
                    continue;
                candidates.push_back({pItem, (uint8)b, (uint8)j, proto->ContainerSlots});
            }
        }

        if (candidates.empty()) break;

        // Sort biggest-first
        std::sort(candidates.begin(), candidates.end(),
            [](const BagCandidate& a, const BagCandidate& b) { return a.size > b.size; });

        for (auto& cand : candidates)
        {
            if (foundAction) break;

            // --- Priority 1: Empty bag slot ---
            for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            {
                if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b))
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-BAGS] %s: equipping [%s] (%u slots) → empty bag slot %d",
                        me->GetName(),
                        cand.item->GetProto()->Name1 ? cand.item->GetProto()->Name1 : "?",
                        cand.size, b);

                    uint16 dest = (INVENTORY_SLOT_BAG_0 << 8) | (uint8)b;
                    me->RemoveItem(cand.bag, cand.slot, false);
                    cand.item->RemoveFromUpdateQueueOf(me);
                    me->EquipItem(dest, cand.item, true);

                    if (!bagEvents.empty()) bagEvents += ",";
                    bagEvents += std::to_string(cand.item->GetEntry()) + ":" + std::to_string(b);

                    changed = true;
                    foundAction = true;
                    break;
                }
            }
            if (foundAction) break;

            // --- Priority 2: Replace smallest EMPTY equipped bag ---
            int worstSlot = -1;
            uint32 worstSize = 999;

            for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            {
                Bag* pBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b);
                if (!pBag) continue;
                uint32 equippedSize = pBag->GetBagSize();
                if (equippedSize >= cand.size) continue;
                if (!pBag->IsEmpty()) continue;

                if (equippedSize < worstSize)
                {
                    worstSize = equippedSize;
                    worstSlot = b;
                }
            }

            if (worstSlot >= 0)
            {
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
                    me->StoreItem(sDest, (Item*)pOldBag, true);
                else
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                        "[AIBOT-BAGS] %s: WARNING could not store old bag %u, destroying",
                        me->GetName(), pOldBag->GetEntry());
                    pOldBag->RemoveFromUpdateQueueOf(me);
                    pOldBag->SetState(ITEM_REMOVED, me);
                }

                if (!bagEvents.empty()) bagEvents += ",";
                bagEvents += std::to_string(cand.item->GetEntry()) + ":" + std::to_string(worstSlot);

                changed = true;
                foundAction = true;
            }
        }

        if (!foundAction) break;
    }

    if (changed)
    {
        // Calculate total slots for the event
        uint32 totalSlots = (INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START);
        for (int i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        {
            Bag* p = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)i);
            if (p && p->GetProto()->Class == ITEM_CLASS_CONTAINER &&
                p->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER)
                totalSlots += p->GetBagSize();
        }

        uint32 freeSlots = 0;
        for (int fi = INVENTORY_SLOT_ITEM_START; fi < INVENTORY_SLOT_ITEM_END; ++fi)
            if (!me->GetItemByPos(INVENTORY_SLOT_BAG_0, fi))
                ++freeSlots;
        for (int fi = INVENTORY_SLOT_BAG_START; fi < INVENTORY_SLOT_BAG_END; ++fi)
            if (Bag* pFBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, fi))
                if (pFBag->GetProto()->Class == ITEM_CLASS_CONTAINER && pFBag->GetProto()->SubClass == ITEM_SUBCLASS_CONTAINER)
                    for (uint32 fj = 0; fj < pFBag->GetBagSize(); ++fj)
                        if (!me->GetItemByPos(fi, fj))
                            ++freeSlots;
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
        case CLASS_WARRIOR: return { 1.6f, 0.7f, 1.2f, 0.0f, 0.0f, 0.55f, 0.0f, 0.0f, 0.0f, 1.1f, 6.0f };
        case CLASS_ROGUE:   return { 1.0f, 1.6f, 0.9f, 0.0f, 0.0f, 0.25f, 0.0f, 0.0f, 0.0f, 1.0f, 6.0f };
        case CLASS_HUNTER:  return { 0.6f, 1.6f, 0.9f, 0.4f, 0.0f, 0.25f, 0.0f, 0.0f, 0.0f, 1.0f, 5.0f };
        case CLASS_PALADIN: return { 1.4f, 0.5f, 1.2f, 0.7f, 0.6f, 0.50f, 0.6f, 0.9f, 0.7f, 1.0f, 5.0f };
        case CLASS_SHAMAN:  return { 1.0f, 0.8f, 1.1f, 1.1f, 0.8f, 0.35f, 1.2f, 1.0f, 0.9f, 0.9f, 4.0f };
        case CLASS_DRUID:   return { 1.0f, 0.9f, 1.1f, 1.1f, 0.8f, 0.20f, 1.2f, 1.0f, 0.9f, 0.9f, 4.0f };
        case CLASS_PRIEST:  return { 0.0f, 0.2f, 0.7f, 1.3f, 1.0f, 0.05f, 1.6f, 1.6f, 1.2f, 0.0f, 1.0f };
        case CLASS_MAGE:    return { 0.0f, 0.2f, 0.7f, 1.3f, 0.8f, 0.05f, 2.0f, 0.0f, 1.0f, 0.0f, 1.0f };
        case CLASS_WARLOCK: return { 0.0f, 0.2f, 0.7f, 1.3f, 0.8f, 0.05f, 2.0f, 0.0f, 1.0f, 0.0f, 1.0f };
        default:            return { 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.30f, 1.0f, 1.0f, 0.8f, 1.0f, 4.0f };
    }
}

// InventoryType → equipment slot (the same mapping the loot need/greed roll does inline).
static uint8 EquipSlotForInvType(uint8 invType)
{
    switch (invType)
    {
        case INVTYPE_HEAD:            return EQUIPMENT_SLOT_HEAD;
        case INVTYPE_NECK:            return EQUIPMENT_SLOT_NECK;
        case INVTYPE_SHOULDERS:       return EQUIPMENT_SLOT_SHOULDERS;
        case INVTYPE_BODY:            return EQUIPMENT_SLOT_BODY;
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:            return EQUIPMENT_SLOT_CHEST;
        case INVTYPE_WAIST:           return EQUIPMENT_SLOT_WAIST;
        case INVTYPE_LEGS:            return EQUIPMENT_SLOT_LEGS;
        case INVTYPE_FEET:            return EQUIPMENT_SLOT_FEET;
        case INVTYPE_WRISTS:          return EQUIPMENT_SLOT_WRISTS;
        case INVTYPE_HANDS:           return EQUIPMENT_SLOT_HANDS;
        case INVTYPE_FINGER:          return EQUIPMENT_SLOT_FINGER1;
        case INVTYPE_TRINKET:         return EQUIPMENT_SLOT_TRINKET1;
        case INVTYPE_CLOAK:           return EQUIPMENT_SLOT_BACK;
        case INVTYPE_WEAPON:
        case INVTYPE_2HWEAPON:
        case INVTYPE_WEAPONMAINHAND:  return EQUIPMENT_SLOT_MAINHAND;
        case INVTYPE_SHIELD:
        case INVTYPE_WEAPONOFFHAND:
        case INVTYPE_HOLDABLE:        return EQUIPMENT_SLOT_OFFHAND;
        case INVTYPE_RANGED:
        case INVTYPE_THROWN:
        case INVTYPE_RANGEDRIGHT:     return EQUIPMENT_SLOT_RANGED;
        default:                      return 0xFF;
    }
}

float AiBotAI::ScoreItem(ItemPrototype const* proto, uint8 slot) const
{
    (void)slot;   // slot is the caller's hint; scoring is slot-independent here
    if (!proto)
        return 0.0f;

    AiBotStatWeights const w = GetClassWeights(me->GetClass());
    float score = 0.0f;

    // 1) Primary stats (ItemStat)
    for (auto const& s : proto->ItemStat)
    {
        if (s.ItemStatValue == 0) continue;
        float v = (float)s.ItemStatValue;
        switch (s.ItemStatType)
        {
            case ITEM_MOD_STRENGTH:  score += v * w.str;   break;
            case ITEM_MOD_AGILITY:   score += v * w.agi;   break;
            case ITEM_MOD_STAMINA:   score += v * w.sta;   break;
            case ITEM_MOD_INTELLECT: score += v * w.intel; break;
            case ITEM_MOD_SPIRIT:    score += v * w.spi;   break;
            default:                 score += v * 0.3f;    break;   // misc, small credit
        }
    }

    // 2) ON-EQUIP spell effects — the vanilla "hidden" budget (spell dmg / healing / AP / mp5).
    //    Walk the item's equip-triggered spells and credit their auras. (§verify the field names.)
    const uint32 ON_EQUIP = 1;   // ITEM_SPELLTRIGGER_ON_EQUIP
    for (int si = 0; si < MAX_ITEM_PROTO_SPELLS; ++si)
    {
        if (proto->Spells[si].SpellTrigger != ON_EQUIP) continue;
        int32 spellId = proto->Spells[si].SpellId;
        if (spellId <= 0) continue;

        SpellEntry const* se = sSpellMgr.GetSpellEntry((uint32)spellId);
        if (!se) continue;

        for (int e = 0; e < 3; ++e)
        {
            if (se->Effect[e] != SPELL_EFFECT_APPLY_AURA) continue;
            float val = (float)(se->EffectBasePoints[e] + 1);   // vanilla stores value-1
            if (val <= 0.0f) continue;

            switch (se->EffectApplyAuraName[e])
            {
                case SPELL_AURA_MOD_DAMAGE_DONE:         score += val * w.spellDmg;   break; // +spell dmg
                case SPELL_AURA_MOD_HEALING_DONE:        score += val * w.healing;    break;
                case SPELL_AURA_MOD_POWER_REGEN:         score += val * w.mp5;        break;
                case SPELL_AURA_MOD_ATTACK_POWER:        score += val * w.ap;         break;
                case SPELL_AURA_MOD_RANGED_ATTACK_POWER: score += val * w.ap;         break;
                case SPELL_AURA_MOD_STAT:                score += val * 0.8f;         break; // +stat
                case SPELL_AURA_MOD_INCREASE_HEALTH:     score += val * w.sta * 0.1f; break;
                case SPELL_AURA_MOD_RESISTANCE:          score += val * 0.5f;         break;
                default: break;
            }
        }
    }

    // 3) Armor — class-scaled, downweighted
    if (proto->Armor > 0)
        score += (float)proto->Armor * w.armor;

    // 4) Shield block
    if (proto->Block > 0)
        score += (float)proto->Block * 0.5f;

    // 5) Weapon white DPS
    if (proto->IsWeapon() && proto->Delay > 0)
    {
        float dmg = 0.0f;
        for (int i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i)
        {
            if (proto->Damage[i].DamageMax == 0.0f) break;
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
        case ITEM_QUALITY_POOR:     qmul = 0.85f; break; // grey
        case ITEM_QUALITY_NORMAL:   qmul = 1.00f; break; // white
        case ITEM_QUALITY_UNCOMMON: qmul = 1.15f; break; // green
        case ITEM_QUALITY_RARE:     qmul = 1.30f; break; // blue
        case ITEM_QUALITY_EPIC:     qmul = 1.45f; break; // purple
        default:                    qmul = 1.50f; break; // legendary+
    }
    score *= qmul;

    return score;
}

void AiBotAI::TryAutoEquip()
{
    if (!me || !me->IsAlive() || !me->IsInWorld() || me->IsInCombat())
        return;
 
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
            if (!pItem) return false;
 
            ItemPrototype const* proto = pItem->GetProto();
            if (!proto) return false;
 
            // Only weapons and armor (bags handled by TryAutoEquipBags)
            if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
                return false;
 
            uint16 dest = 0;
            InventoryResult canEquip = me->CanEquipItem(NULL_SLOT, dest, pItem, true);
            if (canEquip != EQUIP_ERR_OK)
                return false;
 
            uint8 targetSlot = dest & 0xFF;
 
            float newScore = ScoreItem(proto, targetSlot);
 
            float oldScore = 0.0f;
            Item* pOldItem = me->GetItemByPos(INVENTORY_SLOT_BAG_0, targetSlot);
            if (pOldItem)
                oldScore = ScoreItem(pOldItem->GetProto(), targetSlot);
 
            if (newScore <= oldScore)
                return false;
 
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
            {
                // No existing item in target slot — just move new item there
                me->RemoveItem(srcBag, srcSlot, false);
                pItem->RemoveFromUpdateQueueOf(me);
                me->EquipItem(dest, pItem, true);
            }
            else
            {
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
                    me->StoreItem(sDest, pOldItem, true);
                else
                {
                    // Try the slot the new item came from
                    sDest.clear();
                    storeRes = me->CanStoreItem(srcBag, srcSlot, sDest, pOldItem, false);
                    if (storeRes == EQUIP_ERR_OK)
                        me->StoreItem(sDest, pOldItem, true);
                    else
                    {
                        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                            "[AIBOT-EQUIP] %s: WARNING could not store old item %u, destroying",
                            me->GetName(), pOldItem->GetEntry());
                        pOldItem->RemoveFromUpdateQueueOf(me);
                        pOldItem->SetState(ITEM_REMOVED, me);
                    }
                }
            }
 
            me->AutoUnequipOffhandIfNeed();
 
            if (!equipEvents.empty()) equipEvents += ",";
            equipEvents += std::to_string(proto->ItemId) + ":" + std::to_string(targetSlot);
 
            equipped = true;
            return true; // signal restart
        };
 
        // Scan backpack
        for (int i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END && !foundUpgrade; ++i)
            foundUpgrade = checkItem(INVENTORY_SLOT_BAG_0, (uint8)i);
 
        // Scan extra bags
        if (!foundUpgrade)
        {
            for (int b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END && !foundUpgrade; ++b)
            {
                Bag* pBag = (Bag*)me->GetItemByPos(INVENTORY_SLOT_BAG_0, (uint8)b);
                if (!pBag || pBag->GetProto()->Class != ITEM_CLASS_CONTAINER)
                    continue;
                for (uint32 j = 0; j < pBag->GetBagSize() && !foundUpgrade; ++j)
                    foundUpgrade = checkItem((uint8)b, (uint8)j);
            }
        }
 
        if (!foundUpgrade) break;
    }
 
    if (equipped)
    {
        char eventData[256];
        snprintf(eventData, sizeof(eventData), "items=%s", equipEvents.c_str());
        BridgeSendEvent("EQUIP", eventData);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-EQUIP] %s: === DONE === equipped: %s", me->GetName(), equipEvents.c_str());
    }
}