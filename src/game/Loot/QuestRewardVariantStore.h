#ifndef MANGOS_QUESTREWARDVARIANTSTORE_H
#define MANGOS_QUESTREWARDVARIANTSTORE_H

#include "Common.h"
#include <unordered_map>
#include <vector>

struct QuestRewardVariant
{
    uint32 itemId;      // generated variant item_template.entry
    float  weight;      // roll weight (derived from budget_pct: low tier common)
};

class QuestRewardVariantStore
{
public:
    static QuestRewardVariantStore& Instance()
    {
        static QuestRewardVariantStore s;
        return s;
    }

    void Load();        // clear + reload from DB (safe to call at boot and on .reload)

    // Returns a rolled variant for baseItemId, or baseItemId itself if none exist.
    uint32 RollVariant(uint32 baseItemId) const;

    bool HasVariants(uint32 baseItemId) const
    {
        return m_map.find(baseItemId) != m_map.end();
    }

private:
    QuestRewardVariantStore() = default;
    std::unordered_map<uint32, std::vector<QuestRewardVariant>> m_map;
};

#define sQuestRewardVariantStore QuestRewardVariantStore::Instance()

#endif