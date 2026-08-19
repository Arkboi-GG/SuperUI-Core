/* SuperUI shared boot-latched worldstate. */

#include "SuiWorldState.h"

#include "Database/DatabaseEnv.h"
#include "Log.h"

namespace SuiWorldState
{

// ── Worldstate (see SuiWorldState.h for the two-tier rule) ──────────────────

static bool s_rtsWorldState = false;

void LoadWorldState()
{
    s_rtsWorldState = false;
    // MangosSuperUI creates the complete RTS overlay while constructing a
    // World State. A stock characters database has no overlay and boots as MMO
    // without the core creating, altering, or querying an absent RTS table.
    uint32 overlayTableCount = 0;
    if (auto result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
            "AND table_type='BASE TABLE' AND table_name IN "
            "('superui_worldstate','superui_rules_zone','superui_rules_hub','superui_rules_hero',"
            "'superui_rules_dungeon','superui_faction','superui_heroes','superui_zone_control',"
            "'superui_dungeon_control')"))
        overlayTableCount = result->Fetch()[0].GetUInt32();

    if (overlayTableCount == 9)
    {
        if (auto result = CharacterDatabase.Query(
                "SELECT `value` FROM `superui_worldstate` WHERE `key`='mode' LIMIT 1"))
            s_rtsWorldState = result->Fetch()[0].GetCppString() == "rts";
    }
    else if (overlayTableCount != 0)
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "[SUI] incomplete RTS overlay: found %u of 9 required characters tables; RTS disabled",
            overlayTableCount);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[SUI] worldstate: %s",
        s_rtsWorldState ? "RTS OVERLAY ACTIVE" : "vanilla (RTS overlay inactive)");
}

bool RtsWorldState() { return s_rtsWorldState; }

} // namespace SuiWorldState
