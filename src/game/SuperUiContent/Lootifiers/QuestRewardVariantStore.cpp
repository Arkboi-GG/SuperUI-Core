#include "QuestRewardVariantStore.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "ProgressBar.h"
#include "Util.h"          // for irand / rand_norm

void QuestRewardVariantStore::Load()
{
    m_map.clear();                                          // for reload case

    uint32 count = 0;

    // creature_entry = 0 is the quest-reward sentinel written by the C# tool.
    // The tracking table lives in the ADMIN schema (vmangos_admin), which is a
    // SEPARATE database from WorldDatabase's default (mangos) — so it MUST be
    // schema-qualified here, or the query silently returns null and no variants
    // load. budget_pct: higher = rarer variant; converted to a roll WEIGHT the
    // same way the loot pools do (low tier common, "of the Gods"/legendary rare).
    std::unique_ptr<QueryResult> result(WorldDatabase.Query(
        "SELECT base_entry, generated_entry, budget_pct "
        "FROM `vmangos_admin`.`lootifier_generated_items` WHERE creature_entry = 0"));

    if (!result)
    {
        BarGoLink bar(1);
        bar.step();
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "");
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded 0 quest reward variants (none defined).");
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        bar.step();
        Field* fields = result->Fetch();

        uint32 baseId    = fields[0].GetUInt32();
        uint32 variantId = fields[1].GetUInt32();
        float  budgetPct = fields[2].GetFloat();

        if (!baseId || !variantId)
            continue;

        // Weight: PoolWeight-style inverse of budget. max(1, 105 - budget) so a
        // 150% legendary -> weight 1 (rarest), a 20% "Improved" -> weight ~85.
        float weight = 105.0f - budgetPct;
        if (weight < 1.0f) weight = 1.0f;

        m_map[baseId].push_back({ variantId, weight });
        ++count;
    }
    while (result->NextRow());

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "");
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u quest reward variants for %zu base items",
             count, m_map.size());
}

uint32 QuestRewardVariantStore::RollVariant(uint32 baseItemId) const
{
    auto itr = m_map.find(baseItemId);
    if (itr == m_map.end() || itr->second.empty())
        return baseItemId;                                 // not lootified -> base

    std::vector<QuestRewardVariant> const& variants = itr->second;

    float total = 0.0f;
    for (auto const& v : variants)
        total += v.weight;

    if (total <= 0.0f)
        return variants[urand(0, variants.size() - 1)].itemId;

    // Weighted pick. rand_norm() returns [0,1); scale to [0,total).
    float roll = float(rand_norm()) * total;
    for (auto const& v : variants)
    {
        roll -= v.weight;
        if (roll < 0.0f)
            return v.itemId;
    }
    return variants.back().itemId;                         // fp safety net
}