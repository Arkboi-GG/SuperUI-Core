#include "CraftingRewardVariantStore.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "ProgressBar.h"
#include "Util.h"          // rand_norm / urand
#include <cctype>

// ─────────────────────────────────────────────────────────────────────────────
//  Tunable distribution knobs.
//  PoC: hardcoded. Phase 2: load these from a crafting_lootifier_config row so the
//  future UI sliders drive them and a `.reload crafting_variants` re-applies them
//  with NO item regeneration.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr float CRAFT_P_BASE      = 0.20f;  // craft returns the plain base item
static constexpr float CRAFT_P_IMPROVED  = 0.50f;  // Improved band
static constexpr float CRAFT_P_TOPBAND   = 0.30f;  // of Power + of Glory + of the Gods
static constexpr float CRAFT_POWER_SHARE = 0.60f;  // of the non-legendary top-band remainder;
                                                   //   of Glory takes the other 0.40

// Legendary ("of the Gods") ABSOLUTE chance by base quality, carved from the top
// band. Harder-to-craft item = better legendary odds.
static float CraftLegendaryChanceForQuality(uint32 q)
{
    switch (q)
    {
        case 2:  return 0.001f;   // green  ~0.1%
        case 3:  return 0.03f;    // blue   ~3%
        case 4:  return 0.15f;    // purple ~15%
        default: return (q >= 5) ? 0.20f : 0.0f;  // orange+ cap; grey/white none
    }
}

// Tier now comes from the STORED tier_name (written by the C# generator), NOT from
// budget_pct — so a band's boost % is pure magnitude and any range is legal. Match
// by substring so both the canonical tokens ("improved"/"power"/"glory"/"gods") and
// legacy display labels ("of the Gods", "of Power", ...) bucket correctly.
static CraftTier CraftTierFromName(std::string name)
{
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    if (name.find("god") != std::string::npos || name.find("legend") != std::string::npos) return CRAFT_TIER_GODS;
    if (name.find("glory") != std::string::npos) return CRAFT_TIER_GLORY;
    if (name.find("power") != std::string::npos) return CRAFT_TIER_POWER;
    return CRAFT_TIER_IMPROVED;  // "improved" + anything unrecognized
}

void CraftingRewardVariantStore::Load()
{
    m_map.clear();                                     // for the reload case

    uint32 count = 0;

    // creature_entry = -1 is the crafting sentinel written by the C# tool (0 =
    // quest, positive = ARPG creature, negative = crafting). The tracking table
    // lives in the ADMIN schema (vmangos_admin), separate from WorldDatabase's
    // default (mangos), so it MUST be schema-qualified. We join mangos.item_template
    // for the base item's quality (needed by the quality-gated legendary roll);
    // both schemas share the server so one query is fine. The MAX(patch) subquery
    // resolves item_template's (entry, patch) composite to the live row. Bucketing
    // is by tier_name (explicit tier), so budget_pct is no longer read here.
    std::unique_ptr<QueryResult> result(WorldDatabase.Query(
        "SELECT g.base_entry, g.generated_entry, g.tier_name, it.quality "
        "FROM `vmangos_admin`.`lootifier_generated_items` g "
        "JOIN `mangos`.`item_template` it "
        "  ON it.entry = g.base_entry "
        " AND it.patch = (SELECT MAX(it2.patch) FROM `mangos`.`item_template` it2 "
        "                 WHERE it2.entry = g.base_entry) "
        "WHERE g.creature_entry = -1"));

    if (!result)
    {
        BarGoLink bar(1);
        bar.step();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded 0 crafting reward variants (none defined).");
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        bar.step();
        Field* fields = result->Fetch();

        uint32 baseId    = fields[0].GetUInt32();
        uint32 variantId = fields[1].GetUInt32();
        std::string tier = fields[2].GetCppString();
        uint32 quality   = fields[3].GetUInt32();

        if (!baseId || !variantId)
            continue;

        CraftVariantSet& set = m_map[baseId];
        set.baseQuality = quality;
        set.byTier[CraftTierFromName(tier)].push_back(variantId);
        ++count;
    }
    while (result->NextRow());

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u crafting reward variants for %zu base items",
             count, m_map.size());
}

uint32 CraftingRewardVariantStore::RollVariant(uint32 baseItemId) const
{
    auto itr = m_map.find(baseItemId);
    if (itr == m_map.end())
        return baseItemId;                             // not lootified -> base

    CraftVariantSet const& set = itr->second;

    // 1) Base passthrough — ~20% of crafts hand back the plain base item.
    float r = float(rand_norm());                      // [0,1)
    if (r < CRAFT_P_BASE)
        return baseItemId;

    // 2) Absolute band thresholds for THIS base. The legendary share is
    //    quality-gated and carved from the top band; the freed probability goes
    //    to of Power / of Glory.
    float leg = CraftLegendaryChanceForQuality(set.baseQuality);
    if (leg > CRAFT_P_TOPBAND)
        leg = CRAFT_P_TOPBAND;

    float rest  = CRAFT_P_TOPBAND - leg;               // of Power + of Glory
    float power = rest * CRAFT_POWER_SHARE;
    float glory = rest - power;

    float tImproved = CRAFT_P_BASE + CRAFT_P_IMPROVED; // end of Improved band
    float tPower    = tImproved + power;               // end of of Power band
    float tGlory    = tPower + glory;                  // end of of Glory band
    // (tGlory .. 1.0] is the legendary / of the Gods band

    CraftTier band;
    if (r < tImproved)      band = CRAFT_TIER_IMPROVED;
    else if (r < tPower)    band = CRAFT_TIER_POWER;
    else if (r < tGlory)    band = CRAFT_TIER_GLORY;
    else                    band = CRAFT_TIER_GODS;

    // 3) Pick uniformly within the chosen band. If the band is empty, walk DOWN
    //    the ladder to the nearest populated band so a craft never fails to
    //    resolve; then upward as a last resort.
    for (int t = band; t >= 0; --t)
    {
        std::vector<uint32> const& v = set.byTier[t];
        if (!v.empty())
            return v[urand(0, v.size() - 1)];
    }
    for (int t = band + 1; t < CRAFT_TIER_MAX; ++t)
    {
        std::vector<uint32> const& v = set.byTier[t];
        if (!v.empty())
            return v[urand(0, v.size() - 1)];
    }

    return baseItemId;                                 // no variants at all -> base
}