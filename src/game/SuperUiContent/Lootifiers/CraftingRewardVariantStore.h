#ifndef MANGOS_CRAFTINGREWARDVARIANTSTORE_H
#define MANGOS_CRAFTINGREWARDVARIANTSTORE_H

#include "Common.h"
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Crafting Lootifier variant store
//
//  Independent twin of QuestRewardVariantStore. Loads ONLY the crafting-sentinel
//  rows from the shared tracking table (vmangos_admin.lootifier_generated_items
//  WHERE creature_entry = -1) and rolls a variant for a crafted item at
//  Spell::DoCreateItem time.
//
//  Roll model — DIFFERENT from quest/ARPG (which weight by 105 - budget_pct):
//    * ~20% of crafts return the BASE item unchanged (base passthrough).
//    * ~50% land in the Improved band.
//    * ~30% land in the top band, split of Power / of Glory / of the Gods.
//    * "of the Gods" IS the legendary (orange) tier. Its share of the top band is
//      gated by the BASE item's quality — green ~0.1%, blue ~3%, purple ~15% of
//      all crafts (harder-to-craft = better legendary odds). The freed
//      probability falls to of Power / of Glory.
//
//  Variants are bucketed into bands by budget_pct (here budget_pct = the additive
//  BOOST %, not the quest multiplier). Thresholds live in the .cpp and MUST match
//  the C# crafting generator's boost bands. Every distribution constant is a
//  future-UI knob (phase 2: load from a crafting_lootifier_config row).
// ─────────────────────────────────────────────────────────────────────────────

enum CraftTier
{
    CRAFT_TIER_IMPROVED = 0,
    CRAFT_TIER_POWER,
    CRAFT_TIER_GLORY,
    CRAFT_TIER_GODS,        // == legendary (orange, quality 5)
    CRAFT_TIER_MAX
};

struct CraftVariantSet
{
    uint32 baseQuality = 0;                             // item_template.quality of the base item
    std::vector<uint32> byTier[CRAFT_TIER_MAX];         // generated variant entries, bucketed by band
};

class CraftingRewardVariantStore
{
public:
    static CraftingRewardVariantStore& Instance()
    {
        static CraftingRewardVariantStore s;
        return s;
    }

    void Load();        // clear + reload from DB (safe at boot and on .reload crafting_variants)

    // Returns a rolled variant for baseItemId, or baseItemId itself (base passthrough
    // or no variants defined).
    uint32 RollVariant(uint32 baseItemId) const;

    bool HasVariants(uint32 baseItemId) const
    {
        return m_map.find(baseItemId) != m_map.end();
    }

private:
    CraftingRewardVariantStore() = default;
    std::unordered_map<uint32, CraftVariantSet> m_map;
};

#define sCraftingRewardVariantStore CraftingRewardVariantStore::Instance()

#endif
