/* SuperUI RTS AiBot heroes (R2). */

#ifndef MANGOS_SUI_HERO_H
#define MANGOS_SUI_HERO_H

#include "Common.h"

#include <vector>

class Player;
class Unit;

namespace SuiHero
{
    struct Snapshot
    {
        uint32 GuidLow;
        uint8 Team;
        uint8 Level;
        bool Dead;
    };

    // Returns false unless all five configured rules and their native world
    // spell rows are valid. Invalid configuration fails the module closed.
    bool LoadRuleset();
    void Tick();
    void Shutdown();

    void OnUnitKill(Unit* victim);
    void OnPlayerWorldEnter(Player* player);
    bool BlocksResurrection(Player const* player);

    uint16 Fielded(uint8 teamIdx);
    uint16 SlotCap(uint8 teamIdx);
    void SnapshotRows(std::vector<Snapshot>& rows);

    // Actions: 1 declare, 2 upgrade, 3 paid revive. Result codes match 841:
    // 0 ok, 1 honor, 2 slots, 3 subject/state, 4 unsupported/disabled.
    uint8 HandleAction(Player* actor, uint8 action, uint64 subjectRaw, int64& poolAfter);
}

#endif
