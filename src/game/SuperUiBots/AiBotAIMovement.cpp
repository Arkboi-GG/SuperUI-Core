/*
 * AiBotAIMovement.cpp — movement & pathing for the autonomous AI bot.
 *
 * Split from the monolithic AiBotAI.cpp. THIS TU holds the movement domain:
 *   - StopMoving / DoRandomWander
 *   - IsPathSafe (corridor creature-level scan)
 *   - chunked pathing (ClearStoredPath / StartNextPathChunk)
 *   - MoveToDestination (seam-cross / nudge / ring / off-mesh snap / chunk dispatch)
 *   - FindNearestNavmeshPoint (Detour findNearestPoly) + ReGroundZ (the float-maroon fix)
 *   - RecordNavBoundary / FindNavBoundaryNear (learned cave-portal seams)
 *
 * All members of AiBotAI; cross-TU calls (BridgeSendEvent, RecordNavBoundary's peers,
 * ConvertMoveToGrindInPlace) resolve against the Bridge / Grind sibling TUs at link time.
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
#include "MoveMap.h"   // MMAP::MMapFactory for the navmesh nearest-poly query

void AiBotAI::StopMoving()
{
    me->StopMoving();
    me->GetMotionMaster()->Clear();
    me->GetMotionMaster()->MoveIdle();
}

// ============================================================
// PHASE 1: Random wander when idle
// ============================================================

void AiBotAI::DoRandomWander()
{
    if (me->IsMoving() || me->IsInCombat())
        return;

    if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
        return;

    // Wander to a random nearby point every 10-20 seconds
    if (m_wanderTimer > 0)
        return;

    m_wanderTimer = urand(10000, 20000);

    float x, y, z;
    me->GetRandomPoint(me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), 15.0f, x, y, z);
    MovePointRun(AIBOT_POINT_WANDER, x, y, z);
}

bool AiBotAI::IsPathSafe(float destX, float destY, float destZ,
                          float &unsafeX, float &unsafeY, float &unsafeZ,
                          uint32 &dangerLevel)
{
    if (!me || !me->IsInWorld() || !me->GetMap())
        return true; // can't validate — allow the move

    uint32 botLevel = me->GetLevel();
    uint32 maxSafeLevel = botLevel + 3;
    uint32 myMapId = me->GetMapId();

    // Cache bot faction template for hostility checks
    FactionTemplateEntry const* botFTE = me->GetFactionTemplateEntry();

    // ── 1. Generate the real mmap path ──
    PathInfo path(me);
    path.calculate(destX, destY, destZ);

    PathType pType = path.getPathType();
    if (pType & PATHFIND_NOPATH)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PATH] %s: NOPATH to (%.1f, %.1f, %.1f) — allowing (not a safety issue)",
            me->GetName(), destX, destY, destZ);
        return true;
    }

    PointsArray const& points = path.getPath();
    if (points.empty())
        return true;

    // ── 2. Walk waypoints, check creature spawns ──
    // Points are ~6yd apart. Check every 3rd = every ~18yd.
    // 30yd radius covers typical aggro range.
    uint32 const STEP = 3;
    float const CHECK_RADIUS_SQ = 30.0f * 30.0f;

    CreatureDataMap const& allSpawns = sObjectMgr.GetCreatureDataMap();

    for (uint32 i = 0; i < (uint32)points.size(); i += STEP)
    {
        Vector3 const& pt = points[i];

        for (auto const& pair : allSpawns)
        {
            CreatureData const& data = pair.second;

            // Wrong map — skip
            if (data.position.mapId != myMapId)
                continue;

            // Fast 2D distance squared
            float dx = data.position.x - pt.x;
            float dy = data.position.y - pt.y;
            float distSq = dx * dx + dy * dy;
            if (distSq > CHECK_RADIUS_SQ)
                continue;

            // Look up creature template for level
            uint32 entry = data.creature_id[0];
            if (entry == 0)
                continue;

            CreatureInfo const* cInfo = sObjectMgr.GetCreatureTemplate(entry);
            if (!cInfo)
                continue;

            // Skip level-0 triggers/objects
            if (cInfo->level_max == 0)
                continue;

            // Skip critters (rabbits, squirrels, etc.)
            if (cInfo->type == CREATURE_TYPE_CRITTER)
                continue;

            // Skip service NPCs (vendors, trainers, quest givers, etc.)
            if (cInfo->npc_flags & (UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_TRAINER |
                                    UNIT_NPC_FLAG_QUESTGIVER | UNIT_NPC_FLAG_FLIGHTMASTER |
                                    UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_BANKER |
                                    UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_STABLEMASTER))
                continue;

            // Skip non-aggressive creatures (won't aggro the bot)
            if (cInfo->flags_extra & CREATURE_FLAG_EXTRA_NO_AGGRO)
                continue;

            // Skip invisible triggers
            if (cInfo->flags_extra & CREATURE_FLAG_EXTRA_INVISIBLE)
                continue;

            // Skip creatures not hostile to the bot (guards, friendly faction NPCs)
            FactionTemplateEntry const* creatureFTE = sObjectMgr.GetFactionTemplateEntry(cInfo->faction);
            if (creatureFTE && botFTE && !creatureFTE->IsHostileTo(*botFTE))
                continue;

            // ── Level check ──
            if (cInfo->level_max > maxSafeLevel)
            {
                unsafeX = pt.x;
                unsafeY = pt.y;
                unsafeZ = pt.z;
                dangerLevel = cInfo->level_max;

                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-PATH] %s: UNSAFE at waypoint %u/%u (%.1f, %.1f, %.1f) — "
                    "'%s' (entry=%u) lvl %u-%u, bot lvl %u (safe<=%u)",
                    me->GetName(), i, (uint32)points.size(),
                    pt.x, pt.y, pt.z,
                    cInfo->name.c_str(), entry,
                    cInfo->level_min, cInfo->level_max,
                    botLevel, maxSafeLevel);

                return false;
            }
        }
    }

    // ── 3. Also check the final waypoint if not already checked by STEP ──
    uint32 lastIdx = (uint32)points.size() - 1;
    if (lastIdx > 0 && (lastIdx % STEP != 0))
    {
        Vector3 const& pt = points[lastIdx];

        for (auto const& pair : allSpawns)
        {
            CreatureData const& data = pair.second;
            if (data.position.mapId != myMapId)
                continue;

            float dx = data.position.x - pt.x;
            float dy = data.position.y - pt.y;
            if (dx * dx + dy * dy > CHECK_RADIUS_SQ)
                continue;

            uint32 entry = data.creature_id[0];
            if (entry == 0)
                continue;

            CreatureInfo const* cInfo = sObjectMgr.GetCreatureTemplate(entry);
            if (!cInfo || cInfo->level_max == 0 || cInfo->type == CREATURE_TYPE_CRITTER)
                continue;
            if (cInfo->npc_flags & (UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_TRAINER |
                                    UNIT_NPC_FLAG_QUESTGIVER | UNIT_NPC_FLAG_FLIGHTMASTER |
                                    UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_BANKER |
                                    UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_STABLEMASTER))
                continue;
            if (cInfo->flags_extra & (CREATURE_FLAG_EXTRA_NO_AGGRO | CREATURE_FLAG_EXTRA_INVISIBLE))
                continue;

            // Skip creatures not hostile to the bot (guards, friendly faction NPCs)
            FactionTemplateEntry const* creatureFTE = sObjectMgr.GetFactionTemplateEntry(cInfo->faction);
            if (creatureFTE && botFTE && !creatureFTE->IsHostileTo(*botFTE))
                continue;

            if (cInfo->level_max > maxSafeLevel)
            {
                unsafeX = pt.x;
                unsafeY = pt.y;
                unsafeZ = pt.z;
                dangerLevel = cInfo->level_max;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-PATH] %s: UNSAFE at dest (%.1f, %.1f, %.1f) — "
                    "'%s' (entry=%u) lvl %u-%u",
                    me->GetName(), pt.x, pt.y, pt.z,
                    cInfo->name.c_str(), entry,
                    cInfo->level_min, cInfo->level_max);
                return false;
            }
        }
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-PATH] %s: path SAFE — %u waypoints to (%.1f, %.1f, %.1f)",
        me->GetName(), (uint32)points.size(), destX, destY, destZ);

    return true;
}

void AiBotAI::ClearStoredPath()
{
    m_pathWaypoints.clear();
    m_pathIndex = 0;
}

// ============================================================
// NEW METHOD 2: StartNextPathChunk
// Place after ClearStoredPath() in AiBotAI.cpp
//
// Walks forward through m_pathWaypoints starting at m_pathIndex,
// accumulating distance between consecutive waypoints. When the
// accumulated distance reaches AIBOT_PATH_CHUNK_DIST (~200yd) or
// we hit the end of the path, calls MovePoint to that waypoint.
//
// Returns true if MovePoint was issued (more path to walk).
// Returns false if path is complete (m_pathIndex already at end).
// ============================================================
 
bool AiBotAI::StartNextPathChunk()
{
    if (m_pathWaypoints.empty() || m_pathIndex >= (uint32)m_pathWaypoints.size())
        return false;

    float accumulated = 0.0f;
    uint32 targetIdx = m_pathIndex;

    for (uint32 i = m_pathIndex; i + 1 < (uint32)m_pathWaypoints.size(); ++i)
    {
        float dx = m_pathWaypoints[i + 1].x - m_pathWaypoints[i].x;
        float dy = m_pathWaypoints[i + 1].y - m_pathWaypoints[i].y;
        float dz = m_pathWaypoints[i + 1].z - m_pathWaypoints[i].z;
        float segDist = sqrtf(dx * dx + dy * dy + dz * dz);
        accumulated += segDist;
        targetIdx = i + 1;

        if (accumulated >= AIBOT_PATH_CHUNK_DIST)
            break;
    }

    // targetIdx is now either ~200yd ahead or the final waypoint
    Vector3 const& target = m_pathWaypoints[targetIdx];

    // Advance the index past the target so next call starts from here
    m_pathIndex = targetIdx;

    bool isFinalChunk = (targetIdx >= (uint32)m_pathWaypoints.size() - 1);

    // ── Travel-speed pin (2026-06-23) ──
    // The chunk MovePoint used the default speed arg (0), so MoveSplineInit fell back to
    // GetSpeed(MOVE_RUN). The fleet was gliding at ~7x run (a 207yd chunk in ~4s instead of
    // ~28s). Pass an EXPLICIT speed so the chunk spline always walks at run speed, whatever
    // the cause (inflated GetSpeed OR a spline that ignores it). runSpeed/rate are logged raw
    // so an underlying rate inflation is visible: if rate>>1 the speed source must be hunted
    // (combat chase will glide too); if rate~1.0 the spline was the bug and this pin is the fix.
    // (GetSpeed / GetSpeedRate are standard MaNGOS Unit getters — verify on your fork.)
    float const runSpeed = me->GetSpeed(MOVE_RUN);
    float const runRate  = me->GetSpeedRate(MOVE_RUN);
    float       useSpeed = runSpeed;
    if (useSpeed > 9.0f)   // vanilla on-foot run = 7.0; >9 (>~1.3x) is not legit for these L1-5 bots
        useSpeed = 7.0f;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-PATH] %s: chunk MovePoint to waypoint %u/%u (%.1f, %.1f, %.1f) "
        "accumulated=%.0fyd runSpeed=%.1f rate=%.2f useSpeed=%.1f %s",
        me->GetName(), targetIdx, (uint32)m_pathWaypoints.size(),
        target.x, target.y, target.z,
        accumulated, runSpeed, runRate, useSpeed,
        isFinalChunk ? "[FINAL]" : "");

    me->GetMotionMaster()->MovePoint(AIBOT_POINT_TASK_DEST,
        target.x, target.y, target.z, MOVE_PATHFINDING, useSpeed);

    return true;
}

// ============================================================
// MoveToDestination — COMPLETE-METHOD REPLACEMENT (2026-06-22)
//
// CHANGE vs the 2026-06-21 version: an OFF-MESH START recovery added inside the
// no_path failure block, BEFORE the MOVE_FAILED|reason=no_path event is emitted.
//
// WHY: PathInfo::calculate() pathfinds FROM the bot's current position. When the
// bot is standing off the navmesh (water surface / unmeshed tile / z-mismatch), the
// START poly lookup fails and EVERY calculate() returns NOPATH regardless of the
// destination — so every MOVE_TO (trainer, all quests) fails identically as no_path,
// then C# shelves the lot even though nothing is wrong with the destinations. The
// bot is just standing somewhere it can't path FROM (confirmed live: Hu at
// (-9153,-397,69) no_path'd four scattered dests in a row, then killed a mob in
// place — local play fine, travel dead).
//
// FIX: before failing the leg, snap the bot onto the nearest navmesh poly and
// re-path from there (mirrors the proven outbound seam-escape just above:
// NearTeleportTo + immediate recursive MoveToDestination). One attempt per journey
// (m_didNavmeshSnap, reset in BridgeHandleMoveTo) so a genuinely unreachable DEST
// still falls through to a real no_path instead of looping.
// ============================================================

void AiBotAI::MoveToDestination(float destX, float destY, float destZ, bool stopCurrentMovement)
{
    if (me->IsInCombat())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-BRIDGE] %s: in combat, deferring MOVE_TO (task armed for resume)", me->GetName());
        // A fresh MOVE_TO received in combat must leave a RESUMABLE task. Previously this
        // returned without setting m_currentTask.type, so the command was silently dropped:
        // BridgeHandleMoveTo stashes the creatureEntry/kill hints, but it is THIS method that
        // promotes the task to TASK_MOVE_TO. Bailing before that promotion left type at
        // TASK_IDLE (post-Clear from the prior leg), so UpdateAI's out-of-combat resume block
        // never re-issued the move — the bot stood with a stale creatureEntry until C#'s 120s
        // objective deadline shelved the quest. Arm the task here; the OOC TASK_MOVE_TO block
        // (approach scan / re-path) drives it the instant combat ends.
        m_currentTask.type = TASK_MOVE_TO;
        m_currentTask.x = destX;
        m_currentTask.y = destY;
        m_currentTask.z = destZ;
        return;
    }

    // ── OFF-MESH ALARM — re-tare onto valid navmesh BEFORE we try to path ──
    // Pathfinding starts FROM the bot. If the bot is standing off the navmesh (fell off a ledge,
    // a water/edge tile, a z-float, a giver spawn pocket with no poly under it), the START poly
    // lookup fails and EVERY calculate() returns NOPATH no matter the dest — so the old flow burned
    // the whole nudge/ring/seam machinery on a broken START and, worst case, "seam-crossed" the bot
    // to a dest it was already standing on (gap=0), firing a FALSE "arrived" and stranding it N yd
    // from the real NPC (the Ujekawab giver loop @ (-8933.5,-136.5)). Off-mesh is an ALARM, not a
    // pathing puzzle: snap the bot back onto real navmesh and resume. ONE re-tare per journey
    // (m_didNavmeshSnap, reset in BridgeHandleMoveTo) so a genuinely unreachable DEST still falls
    // through to a real no_path on the second pass instead of looping. We only CONSUME the one-shot
    // when we actually move the bot — an on-mesh start leaves it armed for a later off-mesh leg.
    if (!m_didNavmeshSnap)
    {
        float snapX = 0.0f, snapY = 0.0f, snapZ = 0.0f;
        if (FindNearestNavmeshPoint(snapX, snapY, snapZ, AIBOT_NAVMESH_SNAP_SEARCH))
        {
            float off = me->GetDistance2d(snapX, snapY);
            if (off > AIBOT_OFFMESH_EPSILON)
            {
                // Off-mesh, but valid navmesh is within the cap → snap onto it and re-path from
                // solid ground. This is the common recoverable case (NOT a failure).
                m_didNavmeshSnap = true;
                // [GROUND] snapZ is already floor-checked inside FindNearestNavmeshPoint; idempotent here.
                ReGroundZ(snapX, snapY, snapZ, "offmesh-retare");
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-PATH] %s: OFF-MESH start (%.1f,%.1f,%.1f) — re-taring %.1fyd onto navmesh "
                    "(%.1f,%.1f,%.1f), re-pathing to (%.1f,%.1f,%.1f)",
                    me->GetName(), me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(),
                    off, snapX, snapY, snapZ, destX, destY, destZ);
                if (stopCurrentMovement)
                    StopMoving();
                me->NearTeleportTo(snapX, snapY, snapZ, me->GetOrientation());
                MoveToDestination(destX, destY, destZ, true);   // re-path from the on-mesh start
                return;
            }
            // else: nearest poly is under us (≤ epsilon) → start is ON mesh; the DEST is what we
            // resolve below. Do NOT consume the one-shot — leave it for a genuine off-mesh later.
        }
        else
        {
            // ALARM (hard): NO navmesh poly within the search box at all → the bot is genuinely
            // stranded (deep out-of-bounds / fell through the world) and cannot path from here.
            // Last-resort help: port to its authored spawn (always valid + on-mesh) if same-map,
            // then re-path. Cross-map stranded (shouldn't happen for a live bot) falls through to a
            // real no_path and the C# death/recovery handling owns the strand.
            m_didNavmeshSnap = true;
            if (m_spawnMapId == me->GetMapId())
            {
                float sx = m_spawnX, sy = m_spawnY, sz = m_spawnZ;
                ReGroundZ(sx, sy, sz, "stranded-spawn");
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-PATH] %s: STRANDED — no navmesh within %.0fyd of (%.1f,%.1f,%.1f); "
                    "porting to spawn (%.1f,%.1f,%.1f) and re-pathing",
                    me->GetName(), AIBOT_NAVMESH_SNAP_SEARCH,
                    me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), sx, sy, sz);
                if (stopCurrentMovement)
                    StopMoving();
                me->NearTeleportTo(sx, sy, sz, m_spawnO);
                MoveToDestination(destX, destY, destZ, true);
                return;
            }
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-PATH] %s: STRANDED — no navmesh within %.0fyd and spawn is cross-map "
                "(spawn map %u, here %u) — failing to C# recovery",
                me->GetName(), AIBOT_NAVMESH_SNAP_SEARCH, m_spawnMapId, me->GetMapId());
            // fall through → normal pathing will NOPATH → MOVE_FAILED → C# owns it
        }
    }

    float x = destX, y = destY, z = destZ;

    // ── Path safety (Session 15) — checks the corridor we're about to walk ──
    float unsafeX = 0, unsafeY = 0, unsafeZ = 0;
    uint32 dangerLevel = 0;
    if (!IsPathSafe(x, y, z, unsafeX, unsafeY, unsafeZ, dangerLevel))
    {
        char eventData[256];
        snprintf(eventData, sizeof(eventData),
            "dest_x=%.1f|dest_y=%.1f|dest_z=%.1f|"
            "unsafe_x=%.1f|unsafe_y=%.1f|unsafe_z=%.1f|"
            "danger_level=%u|bot_level=%u",
            x, y, z,
            unsafeX, unsafeY, unsafeZ,
            dangerLevel, me->GetLevel());
        BridgeSendEvent("PATH_UNSAFE", eventData);
        return;
    }

    // ── Generate mmap path and check result ──
    float origX = x, origY = y; // save original for ring scan
    PathInfo path(me);
    path.calculate(x, y, z);

    PathType pType = path.getPathType();
    // INCOMPLETE is a valid PARTIAL leg now — only NOPATH needs recovery.
    bool pathOk = !(pType & PATHFIND_NOPATH);

    // ── Navmesh seam (cave mouth): dest is NOPATH but we're right next to it ──
    // Reached only with an ON-MESH start (the off-mesh alarm above already re-tared a bad start),
    // so a NOPATH here within seam range is a genuine unmeshed gap to the dest — cross it.
    if (!pathOk && me->GetDistance2d(origX, origY) <= AIBOT_SEAM_CROSS_DIST)
    {
        // §4: an enriched objective MOVE_TO does NOT teleport into the cave — grind at the mouth.
        if (m_currentTask.creatureEntry != 0)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-PATH] %s: objective MOVE_TO hit seam %.1fyd out — grinding at the mouth (no teleport)",
                me->GetName(), me->GetDistance2d(origX, origY));
            ConvertMoveToGrindInPlace();
            return;
        }

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PATH] %s: NOPATH to (%.1f, %.1f) but only %.1fyd off — navmesh seam, crossing",
            me->GetName(), origX, origY, me->GetDistance2d(origX, origY));

        RecordNavBoundary(origX, origY, z);   // inner = dest; outer = where we stand now

        // [GROUND] The seam dest Z is the deep-loader/objective coord and can float above
        // the cave floor — snap it down before the cross (snap-down-only keeps a real cave
        // interior intact).
        ReGroundZ(origX, origY, z, "seam-cross");
        if (stopCurrentMovement)
            StopMoving();
        me->NearTeleportTo(origX, origY, z, me->GetOrientation());

        BridgeSendEvent("TASK_COMPLETE", "MOVE_TO arrived (seam crossed)");
        m_currentTask.Clear();
        ClearStoredPath();
        return;
    }

    // ── Retry 1: Nudge 3yd toward bot (NOPATH only) ──
    if (!pathOk)
    {
        float dx = me->GetPositionX() - x;
        float dy = me->GetPositionY() - y;
        float len = sqrtf(dx * dx + dy * dy);

        if (len > 0.5f)
        {
            float nudgeDist = 3.0f;
            float nx = x + (dx / len) * nudgeDist;
            float ny = y + (dy / len) * nudgeDist;

            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-PATH] %s: NOPATH to (%.1f, %.1f) — retrying nudged toward bot (%.1f, %.1f)",
                me->GetName(), x, y, nx, ny);

            PathInfo retryPath(me);
            retryPath.calculate(nx, ny, z);
            PathType retryType = retryPath.getPathType();

            if (!(retryType & PATHFIND_NOPATH))
            {
                x = nx;
                y = ny;
                pathOk = true;
                path.calculate(x, y, z);
                pType = path.getPathType();

                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-PATH] %s: nudge toward bot SUCCEEDED — using (%.1f, %.1f)",
                    me->GetName(), x, y);
            }
        }
    }

    // ── Retry 2: Ring scan — 8 points at 3yd around original destination ──
    if (!pathOk)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PATH] %s: nudge failed — trying ring scan around (%.1f, %.1f)",
            me->GetName(), origX, origY);

        float ringDist = 3.0f;
        for (int i = 0; i < 8; ++i)
        {
            float angle = (float)i * (M_PI_F / 4.0f); // 0, 45, 90, ... 315 degrees
            float rx = origX + ringDist * cosf(angle);
            float ry = origY + ringDist * sinf(angle);

            PathInfo ringPath(me);
            ringPath.calculate(rx, ry, z);
            PathType ringType = ringPath.getPathType();

            if (!(ringType & PATHFIND_NOPATH))
            {
                x = rx;
                y = ry;
                pathOk = true;
                path.calculate(x, y, z);
                pType = path.getPathType();

                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-PATH] %s: ring scan hit at angle %d° (%.1f, %.1f) — using it",
                    me->GetName(), i * 45, x, y);
                break;
            }
        }

        if (!pathOk)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-PATH] %s: ring scan found no valid point", me->GetName());
        }
    }

    if (!pathOk)
    {
        // ── Outbound seam escape ──
        // The START is on-mesh (off-mesh was re-tared at the top) but we still can't path OUT —
        // a learned cave-portal seam between us and open terrain. Teleport to the recorded outside
        // anchor and re-path. One per journey.
        if (stopCurrentMovement && !m_didBoundaryExit)
        {
            if (NavBoundary const* b = FindNavBoundaryNear(me->GetPositionX(), me->GetPositionY(), AIBOT_BOUNDARY_SCOPE))
            {
                m_didBoundaryExit = true;
                sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                    "[AIBOT-PATH] %s: no path out — escaping known seam to anchor (%.1f, %.1f), re-pathing",
                    me->GetName(), b->outerX, b->outerY);

                StopMoving();
                // [GROUND] b is const → copy to a local, snap down, then teleport.
                float ez = b->outerZ;
                ReGroundZ(b->outerX, b->outerY, ez, "seam-escape");
                me->NearTeleportTo(b->outerX, b->outerY, ez, me->GetOrientation());
                MoveToDestination(destX, destY, destZ, true);   // re-path from the connected mesh
                return;
            }
        }

        // NOPATH after the off-mesh re-tare + seam + nudge + ring + boundary-escape — the leg is
        // genuinely unreachable (the DEST is bad, not the start). Real no_path → C# defers/shelves.
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PATH] %s: MOVE_FAILED — no_path to (%.1f, %.1f, %.1f)",
            me->GetName(), x, y, z);

        char eventData[256];
        snprintf(eventData, sizeof(eventData),
            "dest_x=%.1f|dest_y=%.1f|dest_z=%.1f|reason=no_path",
            x, y, z);
        BridgeSendEvent("MOVE_FAILED", eventData);
        return;
    }

    PointsArray const& points = path.getPath();

    if (points.empty())
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PATH] %s: MOVE_FAILED — empty waypoint array to (%.1f, %.1f, %.1f)",
            me->GetName(), x, y, z);

        char eventData[256];
        snprintf(eventData, sizeof(eventData),
            "dest_x=%.1f|dest_y=%.1f|dest_z=%.1f|reason=empty_path",
            x, y, z);
        BridgeSendEvent("MOVE_FAILED", eventData);
        return;
    }

    // INCOMPLETE = the path stops at the poly budget, short of the dest.
    bool isPartial = (pType & PATHFIND_INCOMPLETE) != 0;

    // ── Total length of THIS leg's path ──
    float totalDist = 0.0f;
    for (uint32 i = 0; i + 1 < (uint32)points.size(); ++i)
    {
        float pdx = points[i + 1].x - points[i].x;
        float pdy = points[i + 1].y - points[i].y;
        float pdz = points[i + 1].z - points[i].z;
        totalDist += sqrtf(pdx * pdx + pdy * pdy + pdz * pdz);
    }

    if (stopCurrentMovement)
        StopMoving();
    ClearStoredPath();

    // The journey's TRUE destination.
    m_currentTask.type = TASK_MOVE_TO;
    m_currentTask.x = x;
    m_currentTask.y = y;
    m_currentTask.z = z;

    // ── Complete + short path: single MovePoint straight to dest ──
    if (!isPartial && totalDist <= AIBOT_PATH_CHUNK_DIST)
    {
        float useSpeed = me->GetSpeed(MOVE_RUN);
        if (useSpeed > 9.0f) useSpeed = 7.0f;

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-PATH] %s: short path (%.0fyd, %u waypoints) — single MovePoint (speed=%.1f)",
            me->GetName(), totalDist, (uint32)points.size(), useSpeed);

        me->GetMotionMaster()->MovePoint(AIBOT_POINT_TASK_DEST, x, y, z, MOVE_PATHFINDING, useSpeed);
        return;
    }

    // ── Long, or a partial leg: store waypoints, walk in ~200yd chunks ──
    m_pathWaypoints.reserve(points.size());
    for (uint32 i = 0; i < (uint32)points.size(); ++i)
        m_pathWaypoints.push_back(points[i]);
    m_pathIndex = 0;

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-PATH] %s: %s path (%.0fyd, %u waypoints) — chunked pathing",
        me->GetName(), isPartial ? "partial" : "long",
        totalDist, (uint32)m_pathWaypoints.size());

    StartNextPathChunk();
}

// ============================================================
// FindNearestNavmeshPoint — nearest valid navmesh poly point to the bot
//
// Queries the mmap navmesh directly (findNearestPoly) with a generous search box,
// because the SMALL default snap extent inside PathInfo::calculate() is exactly what
// is already failing for an off-mesh bot. Returns the closest on-mesh point within
// searchYards, in MaNGOS world coords.
//
// ⚠ VERIFY against your fork (these are the standard VMaNGOS mmap conventions):
//   1. include "MoveMap.h" at the top of AiBotAI.cpp (for MMAP::MMapFactory).
//      PathFinder.h already pulls the Detour headers (dtNavMeshQuery / dtQueryFilter).
//   2. MMapManager::GetNavMeshQuery(mapId) — per-map, single arg on this fork.
//   3. Recast coordinate order: MaNGOS (x,y,z) ⇄ Detour (y,z,x). PathInfo does this
//      same swizzle internally; if yours differs the snap will teleport to a wrong
//      spot — the [AIBOT-PATH] OFF-MESH log line prints src→dst so a bad swizzle is
//      immediately visible on the first run.
// ============================================================
bool AiBotAI::FindNearestNavmeshPoint(float& outX, float& outY, float& outZ, float searchYards) const
{
    MMAP::MMapManager* mmap = MMAP::MMapFactory::createOrGetMMapManager();
    if (!mmap)
        return false;

    dtNavMeshQuery const* query = mmap->GetNavMeshQuery(me->GetMapId());   // per-map query (single arg on this fork)
    if (!query)
        return false;

    // MaNGOS (x,y,z) → Detour (y,z,x).
    float const center[3]  = { me->GetPositionY(), me->GetPositionZ(), me->GetPositionX() };
    float const extents[3] = { searchYards, 50.0f, searchYards };   // wide vertical to catch z-mismatch

    dtQueryFilter filter;   // default: include all areas
    dtPolyRef nearestPoly = 0;
    float nearestPt[3] = { 0.0f, 0.0f, 0.0f };

    dtStatus st = query->findNearestPoly(center, extents, &filter, &nearestPoly, nearestPt);
    if (dtStatusFailed(st) || nearestPoly == 0)
        return false;

    // Detour (y,z,x) → MaNGOS (x,y,z).
    outX = nearestPt[2];
    outY = nearestPt[0];
    outZ = nearestPt[1];

    // [GROUND] Divergence guard: the nearest poly can FLOAT above the collision hull on
    // steep slopes (Recast lays the walkable surface over the geometry). Snapping a bot
    // onto that poly Z is the float-maroon entry vector. If the poly Z reads above the
    // real floor here, take the floor — snap DOWN only (never up into a cave ceiling).
    // UpdateAllowedPositionZ is const, so no cast needed even though this method is const.
    {
        float ground = outZ;
        me->UpdateAllowedPositionZ(outX, outY, ground);
        if (ground < outZ - 0.5f)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[AIBOT-GROUND] %s: navmesh poly floats — snap Z %.1f -> %.1f (%.1fyd) @ (%.1f, %.1f)",
                me->GetName(), outZ, ground, outZ - ground, outX, outY);
            outZ = ground;
        }
    }

    return true;
}

// ============================================================
// ReGroundZ — snap a teleport destination Z DOWN onto real terrain.
//
// THE float-maroon fix. The navmesh poly surface floats ABOVE the collision hull
// on steep slopes, so a NearTeleportTo to a nav-poly Z (off-mesh snap, seam anchor)
// — or to any stale/echoed Z — leaves the bot hovering. A hovering bot sits >poly-
// search-extents off its start poly, so PathInfo::calculate() NOPATHs from the
// SOURCE and the grind scanner finds nothing reachable → permanent wedge (Opiyuxami
// @ z=291). MovePoint sites already re-ground implicitly (GetRandomPoint runs the
// same UpdateAllowedPositionZ); only the teleports decouple Z from terrain, so every
// one of them routes through here.
//
// UpdateAllowedPositionZ is the core's own ground-snap (Object.cpp:2035, vmap+map,
// z-hint aware), so passing the intended Z resolves a CAVE FLOOR correctly, not just
// the outdoor surface.
//
// SAFETY — snap DOWN only. A genuine float is always ground-BELOW the bot, which is
// what we snap to. If the resolved floor reads ABOVE the request (cave interior /
// under a bridge, where GetHeight can return the surface terrain over the cave) we
// KEEP the request — never shove the bot up into geometry. Logged either way so a
// misbehaving site is one grep on the first run.
// ============================================================
void AiBotAI::ReGroundZ(float x, float y, float& z, const char* tag)
{
    if (!me || !me->GetMap())
        return;

    float const zReq = z;
    float ground = zReq;

    // Core ground-snap (vmap-aware, z-hint bounded search). In-place float& z.
    me->UpdateAllowedPositionZ(x, y, ground);

    if (ground < zReq - 0.5f)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-GROUND] %s: %s re-grounded z %.1f -> %.1f (dropped %.1fyd) @ (%.1f, %.1f)",
            me->GetName(), tag ? tag : "?", zReq, ground, zReq - ground, x, y);
        z = ground;
    }
    else if (ground > zReq + 25.0f)
    {
        // Diagnostic only — floor reads far ABOVE the request (cave/bridge): we KEPT z.
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
            "[AIBOT-GROUND] %s: %s kept z %.1f (floor %.1f, +%.1f above — cave/bridge?) @ (%.1f, %.1f)",
            me->GetName(), tag ? tag : "?", zReq, ground, ground - zReq, x, y);
    }
}

// ============================================================
// MovePointRun — MovePoint with run speed pinned.
// The two travel splines (StartNextPathChunk, MoveToDestination short-path) pin
// GetSpeed(MOVE_RUN) so MoveSplineInit::Launch never takes its velocity==0 → inflated-spline
// fallback (~52yd/s glide, model floating on slopes). Every OTHER MovePoint site used the
// default speed arg (0.0f) and still glided+floated — wander, grind-patrol, stalemate/overpull
// hops, the interact approach. They all route here now. Clamp matches the travel pin: >9
// (>~1.3x run) is not legit for these bots.
// ============================================================
void AiBotAI::MovePointRun(uint32 pointId, float x, float y, float z)
{
    float useSpeed = me->GetSpeed(MOVE_RUN);
    if (useSpeed > 9.0f) useSpeed = 7.0f;
    me->GetMotionMaster()->MovePoint(pointId, x, y, z, MOVE_PATHFINDING, useSpeed);
}

 
void AiBotAI::RecordNavBoundary(float innerX, float innerY, float innerZ)
{
    uint32 mapId = me->GetMapId();
    for (NavBoundary const& b : m_navBoundaries)   // dedup: same seam within 10yd
    {
        if (b.mapId != mapId) continue;
        float dx = b.innerX - innerX, dy = b.innerY - innerY;
        if (dx * dx + dy * dy < 100.0f)
            return;
    }

    NavBoundary nb;
    nb.innerX = innerX; nb.innerY = innerY; nb.innerZ = innerZ;
    nb.outerX = me->GetPositionX();   // where we stand now = connected mesh, outside
    nb.outerY = me->GetPositionY();
    nb.outerZ = me->GetPositionZ();
    nb.mapId  = mapId;
    m_navBoundaries.push_back(nb);

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
        "[AIBOT-PATH] %s: recorded nav seam — inner (%.1f, %.1f, %.1f) outer (%.1f, %.1f, %.1f) map=%u (%zu known)",
        me->GetName(), nb.innerX, nb.innerY, nb.innerZ, nb.outerX, nb.outerY, nb.outerZ, mapId, m_navBoundaries.size());
}

NavBoundary const* AiBotAI::FindNavBoundaryNear(float x, float y, float scope) const
{
    uint32 mapId = me->GetMapId();
    float bestSq = scope * scope;
    NavBoundary const* best = nullptr;
    for (NavBoundary const& b : m_navBoundaries)
    {
        if (b.mapId != mapId) continue;
        float dx = b.innerX - x, dy = b.innerY - y;
        float dSq = dx * dx + dy * dy;
        if (dSq < bestSq) { bestSq = dSq; best = &b; }
    }
    return best;
}
