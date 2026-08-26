#ifndef MANGOS_AIBOT_TALENTS_H
#define MANGOS_AIBOT_TALENTS_H

#include "Common.h"
#include "SharedDefines.h"

#include <vector>

class Player;
struct PlayerBotEntry;

namespace AiBotTalents
{
    enum TalentRepairStatus
    {
        TALENT_REPAIR_DISABLED,
        TALENT_REPAIR_ALIGNED,
        TALENT_REPAIR_UPDATED,
        TALENT_REPAIR_CONFLICT,
        TALENT_REPAIR_INVALID_PROFILE,
        TALENT_REPAIR_ERROR,
    };

    struct RepairResult
    {
        RepairResult()
            : status(TALENT_REPAIR_ERROR), learnedPoints(0),
              role(ROLE_INVALID), metadataChanged(false)
        {
        }

        TalentRepairStatus status;
        uint32 learnedPoints;
        CombatBotRoles role;
        bool metadataChanged;
    };

    enum TalentApplyStatus
    {
        TALENT_APPLY_OK,
        TALENT_APPLY_DISABLED,
        TALENT_APPLY_INVALID_PROFILE,
        TALENT_APPLY_INVALID_ROLE,
        TALENT_APPLY_RESET_REQUIRED,
        TALENT_APPLY_FAILED,
        TALENT_APPLY_ROLLBACK_FAILED,
    };

    struct TalentSnapshotEntry
    {
        uint32 talentId;
        uint8 rank;
    };

    struct TalentSnapshot
    {
        TalentSnapshot()
            : specTab(255), activeRole(ROLE_INVALID), profileState(0)
        {
        }

        std::vector<TalentSnapshotEntry> talents;
        uint8 specTab;
        CombatBotRoles activeRole;
        uint8 profileState;
    };

    struct ApplyResult
    {
        ApplyResult()
            : status(TALENT_APPLY_FAILED), learnedPoints(0), removedPoints(0),
              role(ROLE_INVALID), resetPerformed(false), rollbackSucceeded(false)
        {
        }

        TalentApplyStatus status;
        uint32 learnedPoints;
        uint32 removedPoints;
        CombatBotRoles role;
        bool resetPerformed;
        bool rollbackSucceeded;
    };

    // Validates the compiled schema-v1/build-5875 manifest against the DBCs
    // installed by this core. Validation is cached and spending is disabled if
    // any profile has drifted.
    bool ValidateProfiles();

    // Assigns stable metadata when needed, preserves conflicting builds, and
    // spends only legal missing points through Player::LearnTalent.
    RepairResult EnsureProfileAndTalents(Player* player, PlayerBotEntry* entry);

    // Validates and applies one class-local profile/role selection. A spec change
    // is never implicit: callers must request resetTalents, which performs a free
    // reset, buys the selected level-appropriate prefix, and rolls back exactly on
    // failure. Same-spec role/rotation-only changes can pass resetTalents=false.
    ApplyResult ApplyProfileAndRole(Player* player, PlayerBotEntry* entry,
        uint8 specTab, CombatBotRoles role, bool resetTalents);

    bool CaptureSnapshot(Player const* player, PlayerBotEntry const* entry, TalentSnapshot& snapshot);
    bool RestoreSnapshot(Player* player, PlayerBotEntry* entry, TalentSnapshot const& snapshot);
    bool IsProfileRoleAllowed(uint8 classId, uint8 specTab, CombatBotRoles role);
    CombatBotRoles GetDefaultRole(uint8 classId, uint8 specTab, uint32 guidLow);
    char const* GetApplyStatusCode(TalentApplyStatus status);

    char const* GetProfileName(uint8 classId, uint8 specTab);
}

#endif
