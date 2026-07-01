/*
 * AiBotAIMain.h — Autonomous AI bot for VMaNGOS 1.12.1
 *
 * Forked from BattleBotAI. Replaces BG logic with a task executor
 * state machine + TCP bridge for C# BotBrainService communication.
 *
 * Phase 1: Wander, fight, eat/drink, self-revive. ✓
 * Phase 2: TCP bridge on port 3444 — HELLO, STATE, EVENT, inbound commands.
 * Phase 2.5: TASK_GRIND — area grind with proactive creature scanning.
 * Session 25: Chunked pathing — mmap waypoints fed in ~200yd chunks via MovePoint.
 * Session 36: Lossless outbound bridge — BridgeSend queues whole lines,
 *             BridgeFlush drains with partial-write/EWOULDBLOCK tolerance.
 *             Fixes silent event loss (dropped TASK_COMPLETE/KILL/LOOT) under load.
 * [TEAMPLAY]: CombatDirective — the per-member group focus-fire stamp (assist seam).
 *
 * ──────────────────────────────────────────────────────────────────────────
 * FILE SPLIT (this header is the single class declaration; the implementation
 * is divided across per-concern translation units — see each .cpp):
 *
 *   AiBotAIMain.cpp     lifecycle + main loop: OnSessionLoaded / OnPlayerLogin /
 *                       OnPacketReceived / MovementInform / UpdateAI
 *   AiBotAICombat.cpp   targeting + per-class combat (18 verbatim BattleBotAI
 *                       methods + dispatchers), AttackStart / SelectAttackTarget /
 *                       CheckForUnreachableTarget, stalemate + overpull discipline,
 *                       self-maintenance (mount / eat-drink)
 *   AiBotAIBridge.cpp   TCP bridge (connect/send/recv/flush), JsonExtract* helpers,
 *                       BridgeProcessLine + every BridgeHandle* command handler,
 *                       Send*Event emitters
 *   AiBotAIMovement.cpp pathing + navmesh: MoveToDestination, chunked pathing,
 *                       IsPathSafe, ReGroundZ, nav-boundary record/find,
 *                       FindNearestNavmeshPoint, StopMoving, DoRandomWander
 *   AiBotAILoot.cpp     DoAutoLoot, TryAutoEquip(Bags), ScoreItem (+ class weights /
 *                       slot map), ChooseQuestReward
 *   AiBotAIGrind.cpp    SelectGrindTarget, DoGrindPatrol, CountNearbyHostiles,
 *                       ScanApproachTarget, ConvertMoveToGrindInPlace
 *
 * AiBotAI.h is now a one-line compatibility shim (#include this) so existing
 * include sites (AiBotAITeamPlay.*, PlayerBotMgr, the factory) are unchanged.
 * ──────────────────────────────────────────────────────────────────────────
 */

#ifndef MANGOS_AIBOTAI_H
#define MANGOS_AIBOTAI_H

#include "CombatBotBaseAI.h"
#include "Timer.h"
#include "Log.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET BridgeSocket;
  #define BRIDGE_INVALID_SOCKET INVALID_SOCKET
  #define BRIDGE_CLOSE_SOCKET closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int BridgeSocket;
  #define BRIDGE_INVALID_SOCKET (-1)
  #define BRIDGE_CLOSE_SOCKET close
#endif

#define AIBOT_UPDATE_INTERVAL 1000

// Bridge config
#define BRIDGE_HOST "127.0.0.1"
#define BRIDGE_PORT 3444
#define BRIDGE_STATE_INTERVAL 5000       // ms between STATE messages
#define BRIDGE_RECONNECT_BASE 2000       // ms initial reconnect delay
#define BRIDGE_RECONNECT_MAX  30000      // ms max reconnect delay
#define BRIDGE_RECV_BUF_SIZE  4096       // inbound buffer
#define BRIDGE_SEND_BUF_MAX   65536      // outbound queue cap (bytes) — drop oldest past this

// Movement point IDs
#define AIBOT_POINT_WANDER          100
#define AIBOT_POINT_TASK_DEST       101
#define AIBOT_POINT_GRIND_PATROL    102
#define AIBOT_POINT_TAXI_APPROACH   103
#define AIBOT_POINT_STALEMATE_NUDGE 104  // short hop to break an in-combat stalemate
#define AIBOT_POINT_OVERPULL_FLEE   105  // retreat hop away from a too-dense pull
#define AIBOT_ARRIVE_JITTER_MIN 0.2f     // never path to the EXACT dest coord (it can land on a bad poly / seam edge → off-mesh).
#define AIBOT_ARRIVE_JITTER_MAX 2.0f     //   instead resolve to a validated point this far out, random angle — fans bots out + dodges the bad poly.
#define AIBOT_ARRIVE_JITTER_TRIES 16     // ring samples to try before giving up and using the exact coord (lets a real no_path surface).

// Chunked pathing config (Session 25)
#define AIBOT_PATH_CHUNK_DIST 200.0f     // max yards per MovePoint chunk

// Navmesh-seam crossing (overworld caves — interior mesh that doesn't stitch to terrain)
#define AIBOT_SEAM_CROSS_DIST 12.0f      // a NOPATH dest within this = a seam → teleport across
#define AIBOT_OFFMESH_EPSILON  2.0f      // bot within this of a navmesh poly = ON mesh; beyond = off-mesh ALARM → re-tare
#define AIBOT_BOUNDARY_SCOPE  60.0f      // a recorded seam helps escape from anywhere within this of its inner point
#define AIBOT_NAVMESH_SNAP_SEARCH 40.0f  // how far out to look for a navmesh poly to snap an off-mesh bot onto

// Combat-stalemate breaker (in-combat, neither side dealing damage — navmesh seam / unreachable mob)
#define AIBOT_STALEMATE_NUDGE_MS      3000   // no-damage-in-combat time before each nudge
#define AIBOT_STALEMATE_MAX_NUDGES    3      // nudges before forced disengage
#define AIBOT_STALEMATE_NUDGE_DIST    6.0f   // hop distance
#define AIBOT_STALEMATE_NUDGE_HOLD_MS 2000   // let the hop run uninterrupted this long
#define AIBOT_STALEMATE_IGNORE_MS     60000  // ignore the unreachable guid this long after disengage
#define AIBOT_TAPPED_IGNORE_MS  8000   // after disengaging a foreign-tapped mob, ignore its guid this long (don't re-acquire)
// Stage-2 extraction (CombatStop-in-place doesn't free a still-in-range mob — flee, then teleport)
#define AIBOT_STALEMATE_MAX_DISENGAGES 3     // flee attempts before a hard teleport out
#define AIBOT_STALEMATE_FLEE_DIST      30.0f // real distance to flee out of the mob's reach/leash
#define AIBOT_STALEMATE_FLEE_HOLD_MS   2500  // let the flee hop run uninterrupted this long

// Overpull discipline (solo bots must not bum-rush dense packs / respawn fields)
#define AIBOT_OVERPULL_SOLO         3       // solo: > this many mobs on/around me => bail / don't pull
#define AIBOT_OVERPULL_GROUP        6       // grouped: the pack can hold a deeper pull
#define AIBOT_PULL_DENSITY_RADIUS   20.0f   // hostiles within this of a target = "what wakes when I pull it"
#define AIBOT_OVERPULL_RETREAT_DIST 30.0f   // how far to retreat away from the hostile centroid
#define AIBOT_OVERPULL_FLEE_HOLD_MS 2500    // let a retreat hop run uninterrupted this long
#define AIBOT_OVERPULL_MAX_FLEES    4       // retreats before we stop thrashing and let the fight resolve
#define AIBOT_GRIND_SCAN_YARDS   100.0f  // indefinite grind: bot-centric scan radius for the nearest valid XP mob
#define AIBOT_GRIND_HIGH_OFFSET  3       // …skip a mob more than this many levels above the bot (no red suicide)
#define AIBOT_GRIND_FREEZE_DWELL    3      // consecutive frozen grind ticks (~1s each) — over-cap veto OR no valid target — before handing back to C# (GRIND_BLOCKED). ~3s, fast vs the old 8-hold dwell.

// Spell / item ids referenced across the combat + self-maintenance methods.
// (Moved here from AiBotAI.cpp at the file split so every TU — combat, movement,
// loot — sees the same constants; combat uses AB_SPELL_AUTO_SHOT / AB_SPELL_SHOOT_WAND
// / the BG-flag auras, the self-maintenance helpers use the mount + food/drink ids.)
enum AiBotSpells
{
    AB_SPELL_FOOD = 1131,
    AB_SPELL_DRINK = 1137,
    AB_SPELL_AUTO_SHOT = 75,
    AB_SPELL_SHOOT_WAND = 5019,

    AB_SPELL_MOUNT_40_HUMAN = 470,
    AB_SPELL_MOUNT_40_NELF = 10787,
    AB_SPELL_MOUNT_40_DWARF = 6896,
    AB_SPELL_MOUNT_40_GNOME = 17456,
    AB_SPELL_MOUNT_40_TROLL = 10795,
    AB_SPELL_MOUNT_40_ORC = 581,
    AB_SPELL_MOUNT_40_TAUREN = 18363,
    AB_SPELL_MOUNT_40_UNDEAD = 8980,

    AB_SPELL_MOUNT_60_HUMAN = 22717,
    AB_SPELL_MOUNT_60_NELF = 22723,
    AB_SPELL_MOUNT_60_DWARF = 22720,
    AB_SPELL_MOUNT_60_GNOME = 22719,
    AB_SPELL_MOUNT_60_TROLL = 22721,
    AB_SPELL_MOUNT_60_ORC = 22724,
    AB_SPELL_MOUNT_60_TAUREN = 22718,
    AB_SPELL_MOUNT_60_UNDEAD = 22722,

    AB_SPELL_MOUNT_40_PALADIN = 13819,
    AB_SPELL_MOUNT_60_PALADIN = 23214,

    AB_SPELL_MOUNT_40_WARLOCK = 5784,
    AB_SPELL_MOUNT_60_WARLOCK = 23161,

    AB_ITEM_ARROW  = 2512,
    AB_ITEM_BULLET = 2516,

    // NOTE: the BG flag auras (AURA_WARSONG_FLAG / AURA_SILVERWING_FLAG) are intentionally
    // NOT declared here. BattleBotAI.h already defines them (enum FlagSpellsWS), and TUs that
    // include both this header and BattleBotAI.h (e.g. PlayerBotMgr.cpp) would get a redeclaration
    // conflict. They're referenced only by the combat method bodies, so they live file-local in
    // AiBotAICombat.cpp instead — see the anonymous enum at the top of that TU.
};

// Task types for the state machine
enum AiBotTask : uint8
{
    TASK_IDLE = 0,           // Wander, emote, look alive
    TASK_MOVE_TO,            // Pathfind to destination, report arrival
    TASK_QUEST_PICKUP,       // Walk to NPC, accept quest
    TASK_QUEST_KILL,         // Move to area, kill targets, track progress
    TASK_QUEST_GATHER,       // Move to area, interact with game objects
    TASK_QUEST_TURNIN,       // Walk to NPC, turn in quest
    TASK_FOLLOW_PLAYER,      // Grouped with real player — follow mode
    TASK_TRAVEL,             // Long-distance zone transition
    TASK_REST,               // Sit at inn, eat/drink, idle RP behavior
    TASK_GRIND,              // Area grind: patrol radius, kill creature_entry, count down
    TASK_TAXI,               // Flight path: teleport to source node, fly to destination
};

// Current task data (payload from bridge or default)
struct AiBotTaskData
{
    AiBotTask type       = TASK_IDLE;
    uint32 questId       = 0;
    float x              = 0.0f;
    float y              = 0.0f;
    float z              = 0.0f;
    float radius         = 0.0f;
    uint32 npcGuid       = 0;

    // TASK_GRIND fields
    uint32 creatureEntry = 0;      // 0 = kill anything hostile. The PRIMARY objective entry —
                                    // this alone still drives the dispatch coordinate (where the
                                    // bot walks / the grind-in-place center / the Held-objective
                                    // stamp for the 1c reconcile). Nothing about navigation changes.
    int32  killGoal      = 0;      // total kills needed (0 = indefinite)
    int32  killCount     = 0;      // kills so far

    // Alternate entries that satisfy the SAME objective as creatureEntry — for ITEM-DROP
    // objectives only, where multiple creature species genuinely tie on drop odds for the
    // required item (e.g. Young Wolf + Timber Wolf both drop Tough Wolf Meat at the same
    // chance). C# resolves the tie and ships the alternates here on MOVE_TO/SET_TASK
    // (alt_entry1/2/3). Deliberately NOT used for kill objectives — a kill quest names one
    // specific creature and the server only credits kills of that exact entry, so widening
    // match there would let the bot "believe" it's progressing on kills the server never
    // credits. 0 = unused slot. Always 0 unless C# explicitly ships a real tie.
    static const int MAX_ALT_ENTRIES = 3;
    uint32 altCreatureEntries[MAX_ALT_ENTRIES] = { 0, 0, 0 };

    // TASK_TAXI fields
    uint32 taxiSourceNode = 0;
    uint32 taxiDestNode   = 0;

    // True if `entry` is a valid kill for the CURRENT objective — creatureEntry itself, or
    // any non-zero altCreatureEntries slot. Used everywhere the old code checked
    // `entry == creatureEntry`: ScanApproachTarget's valid-kill union, SelectGrindTarget's
    // aggro match + objective ring-scan, and the kill-credit check in UpdateAI. entry==0 is
    // never a match (0 means "no entry / not a creature kill" at every call site).
    bool MatchesObjectiveEntry(uint32 entry) const
    {
        if (entry == 0)
            return false;
        if (entry == creatureEntry)
            return true;
        for (int i = 0; i < MAX_ALT_ENTRIES; ++i)
            if (altCreatureEntries[i] == entry)
                return true;
        return false;
    }

    void Clear()
    {
        type = TASK_IDLE;
        questId = 0;
        x = y = z = radius = 0.0f;
        npcGuid = 0;
        creatureEntry = 0;
        killGoal = 0;
        killCount = 0;
        for (int i = 0; i < MAX_ALT_ENTRIES; ++i)
            altCreatureEntries[i] = 0;
        taxiSourceNode = 0;
        taxiDestNode = 0;
    }
};

// A learned navmesh discontinuity (e.g. an overworld cave mouth where the
// interior mesh doesn't connect to the terrain at the portal). Recorded the
// first time this bot crosses it inbound (teleport), then reused to escape
// outbound. World geometry, but kept per-bot in memory for now (the durable,
// fleet-shared version persists these via a bridge event + C# table).
struct NavBoundary
{
    float innerX, innerY, innerZ;   // the off-mesh dest we crossed TO (inside)
    float outerX, outerY, outerZ;   // the connected-mesh point we crossed FROM (outside)
    uint32 mapId;
};

// ============================================================
// [TEAMPLAY] Per-member combat directive — the group focus-fire seam.
//
// The god-bot coordinator (C#) stamps one of these on each grouped member every brain tick
// via COMBAT_DIRECTIVE. It is STATE (the bot's job) — the stateless TeamPlay resolvers
// (AiBotAITeamPlay.cpp) only READ it; they never write it, never move the bot, never touch
// the wire. v1 carries ONE mode (assist) + the anchor's low GUID. Richer roles
// (interrupt / move-to-ally / focus) arrive as NEW WIRE KEYS when their behaviour ships —
// they are documented intent here, deliberately NOT declared as speculative dead fields
// (that is exactly the debt the C# cleanup just removed).
// ============================================================
enum AiBotCombatMode : uint8
{
    COMBAT_MODE_NONE   = 0,
    COMBAT_MODE_ASSIST = 1,
    // future: COMBAT_MODE_FOCUS, COMBAT_MODE_INTERRUPT, … (add WITH the behaviour, not before)
};

struct CombatDirective
{
    AiBotCombatMode mode = COMBAT_MODE_NONE;
    uint32 anchorGuidLow = 0;
    // RESERVED INTENT (do not declare as fields until the behaviour ships):
    //   role / focusGuidLow / interruptGuidLow / moveToAllyGuidLow
    bool IsActive() const { return mode != COMBAT_MODE_NONE; }
    void Clear()          { mode = COMBAT_MODE_NONE; anchorGuidLow = 0; }
};

class AiBotAI : public CombatBotBaseAI
{
public:
    AiBotAI(uint8 race, uint8 cls, uint32 level, uint32 mapId, uint32 instanceId,
            float x, float y, float z, float o)
        : CombatBotBaseAI(),
          m_spawnRace(race), m_spawnClass(cls), m_spawnLevel(level),
          m_spawnMapId(mapId), m_spawnInstanceId(instanceId),
          m_spawnX(x), m_spawnY(y), m_spawnZ(z), m_spawnO(o),
          m_bridgeSocket(BRIDGE_INVALID_SOCKET)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[AIBOT] AiBotAI constructor fired (factory creation)");
        m_updateTimer.Reset(2000);
    }

    ~AiBotAI()
    {
        BridgeDisconnect();
    }

    bool OnSessionLoaded(PlayerBotEntry* entry, WorldSession* sess) override;
    // --- Lifecycle ---
    void OnPlayerLogin() override;
    void UpdateAI(uint32 const diff) override;
    void OnPacketReceived(WorldPacket const* packet) override;
    void MovementInform(uint32 MovementType, uint32 Data = 0) override;

    // --- Combat (from BattleBotAI) ---
    bool AttackStart(Unit* pVictim);
    Unit* SelectAttackTarget(Unit* pExcept = nullptr) const;
    bool DrinkAndEat();
    bool UseMount();
    uint32 GetMountSpellId() const;
    float GetMaxAggroDistanceForMap() const;
    bool CheckForUnreachableTarget();
    void StopMoving();

    // --- Combat stalemate breaker (Session: in-combat no-damage deadlock) ---
    // HandleCombatStalemate runs per-tick while in combat: detects a no-damage-either-way
    // deadlock (bot on a navmesh seam / mob unreachable across geometry), nudges to break
    // it, and after AIBOT_STALEMATE_MAX_NUDGES force-disengages + ignores the offending guid.
    // IsCombatIgnored is consulted by Select*Target so a freshly-disengaged mob isn't
    // instantly re-acquired from the hostile-ref list. Returns true when it acted this tick.
    bool HandleCombatStalemate();
    bool IsCombatIgnored(uint32 guidLow) const;

    // --- Overpull discipline (solo death-spiral fix) ---
    // OverpullGuard: solo gate consulted BEFORE engaging a grind target — true = the target
    //   sits in too dense a cluster to pull alone, so hold instead of diving in.
    // HandleOverpullRetreat: per-tick in-combat — if more than the cap are on us, AttackStop
    //   and retreat away from the hostile centroid; short-ignore the attackers so we don't
    //   instantly re-acquire them mid-flee. Returns true when it acted (caller returns).
    bool OverpullGuard(Unit* target) const;
    bool HandleOverpullRetreat();

    // --- [TEAMPLAY] public seams for the stateless group-combat resolvers ---
    // TeamPlay (AiBotAITeamPlay.cpp) reads this AI but mutates nothing; these two accessors
    // (+ the public m_combatDirective, beside m_currentTask) are the ONLY surfaces it needs,
    // so it never reaches into protected combat internals. GetBotPlayer keeps the seam
    // base-access-agnostic (works whether or not the base exposes `me` publicly).
    Player* GetBotPlayer() const { return me; }
    bool    IsValidAssistTarget(Unit* pTarget) const;

    // --- 18 pure virtual combat method overrides (verbatim from BattleBotAI) ---
    void UpdateInCombatAI() override;
    void UpdateOutOfCombatAI() override;
    void UpdateInCombatAI_Paladin() override;
    void UpdateOutOfCombatAI_Paladin() override;
    void UpdateInCombatAI_Shaman() override;
    void UpdateOutOfCombatAI_Shaman() override;
    void UpdateInCombatAI_Hunter() override;
    void UpdateOutOfCombatAI_Hunter() override;
    void UpdateInCombatAI_Mage() override;
    void UpdateOutOfCombatAI_Mage() override;
    void UpdateInCombatAI_Priest() override;
    void UpdateOutOfCombatAI_Priest() override;
    void UpdateInCombatAI_Warlock() override;
    void UpdateOutOfCombatAI_Warlock() override;
    void UpdateInCombatAI_Warrior() override;
    void UpdateOutOfCombatAI_Warrior() override;
    void UpdateInCombatAI_Rogue() override;
    void UpdateOutOfCombatAI_Rogue() override;
    void UpdateInCombatAI_Druid() override;
    void UpdateOutOfCombatAI_Druid() override;

    // --- Phase 1 idle wander ---
    void DoRandomWander();

    // --- Auto-loot after kill ---
    void DoAutoLoot(ObjectGuid creatureGuid);
    void TryAutoEquipBags();
    void TryAutoEquip();
    float ScoreItem(ItemPrototype const* proto, uint8 slot) const;

    // --- TASK_GRIND: area grind behavior ---
    void BridgeHandleSetTask(const char* json);
    void BridgeHandleCombatDirective(const char* json);   // [TEAMPLAY] group focus-fire stamp
    void DoGrindPatrol();
    Unit* SelectGrindTarget() const;

    // --- §4 objective approach: enrich MOVE_TO → hand off to GRIND in place ---
    // ScanApproachTarget scans for m_currentTask.creatureEntry around the BOT (no
    // center-radius filter — the center is the deep loader coord). ConvertMoveToGrindInPlace
    // re-centers the grind on the bot's current position so SelectGrindTarget's center
    // filter passes for the local cluster, then leaves the grind to count kills + complete.
    Unit* ScanApproachTarget();
    void  ConvertMoveToGrindInPlace();

    // --- Path safety validation (Session 15) ---
    bool IsPathSafe(float destX, float destY, float destZ,
                    float &unsafeX, float &unsafeY, float &unsafeZ,
                    uint32 &dangerLevel);

    // --- Chunked pathing (Session 25) ---
    // Advances m_pathIndex forward through m_pathWaypoints by up to
    // AIBOT_PATH_CHUNK_DIST yards and issues MovePoint to that waypoint.
    // Returns true if a MovePoint was issued, false if path is complete.
    bool StartNextPathChunk();
    void ClearStoredPath();

    // --- TCP Bridge (Phase 2) ---
    void BridgeConnect();
    void BridgeDisconnect();
    void BridgeSend(const char* json);   // queues a complete JSON line (never truncates)
    void BridgeFlush();                  // drains m_bridgeSendBuf; tolerates partial / EWOULDBLOCK writes
    void BridgeSendHello();
    void BridgeSendState();
    void BridgeSendEvent(const char* eventType, const char* data);
    void BridgeRecv();
    void BridgeProcessLine(const char* line);
    void BridgeHandleMoveTo(const char* json);
    void BridgeHandleTeleport(const char* json);   // generic live-bot teleport (assist + future hearth)
    void MoveToDestination(float destX, float destY, float destZ, bool stopCurrentMovement = true);
    bool FindNearestNavmeshPoint(float& outX, float& outY, float& outZ, float searchYards) const;
    // [GROUND] Snap a teleport/snap destination Z DOWN onto real terrain (the float-maroon
    // fix). Called before EVERY NearTeleportTo so a bot can't be planted on a floating
    // navmesh poly (Recast lays the walkable surface ABOVE the collision hull on steep
    // slopes). Snap-down-only so it can never shove a bot up into geometry.
    void ReGroundZ(float x, float y, float& z, const char* tag = nullptr);
    // Pin run speed on a MovePoint so the spline never takes MoveSplineInit's velocity==0
    // inflated-spline fallback (~52yd/s glide + the model floating off slopes). The two travel
    // splines pin this already; every OTHER mover routes through here so none can forget.
    void MovePointRun(uint32 pointId, float x, float y, float z);
    // Navmesh-seam crossing (cave entry/exit). RecordNavBoundary stores the seam the
    // first time we teleport across inbound; FindNavBoundaryNear lets the outbound
    // leg find the recorded outside anchor to escape back to the connected mesh.
    void RecordNavBoundary(float innerX, float innerY, float innerZ);
    NavBoundary const* FindNavBoundaryNear(float x, float y, float scope) const;
    void BridgeHandleSayText(const char* json);
    void BridgeHandleQuestInteract(const char* json);
    void BridgeHandleAbandonQuest(const char* json);
    void BridgeHandleLearnSpell(const char* json);
    void BridgeHandleAttackTarget(const char* json);
    void BridgeHandleInteractNpc(const char* json);
    void BridgeHandleTakeFlight(const char* json);
    void BridgeHandleSellItems(const char* json);
    void BridgeHandleResurrect(const char* json);
    void BridgeHandleTrain(const char* json);
    void BridgeHandleQueryQuestStatus(const char* json);
    void BridgeHandleUseGameObject(const char* json);
    void BridgeHandleFormGroup(const char* json);
    void BridgeHandleDisbandGroup(const char* json);
    void BridgeHandleRepairItems(const char* json);

    // --- Quest/combat/event helpers ---
    void SendKillEvent(uint32 creatureEntry, uint32 creatureGuidLow);
    void SendQuestUpdateEvent(uint32 questId, const char* status);
    void SendLevelUpEvent(uint32 newLevel);
    void SendChatRecvEvent(const char* senderName, const char* message, const char* chatType, const char* channelName = nullptr);

    // --- Task executor ---
    AiBotTaskData m_currentTask;

    // [TEAMPLAY] Live group combat stamp (read by TeamPlay::ResolveCombatTarget).
    // Public like the rest of this AI's state; stamped by BridgeHandleCombatDirective,
    // cleared on a 'none'/absent directive. Inactive (IsActive()==false) on every solo bot,
    // so the combat seam is a provable no-op unless the coordinator stamps it.
    CombatDirective m_combatDirective;

    // --- Victim Tracking ---
    uint32 m_lastVictimEntry = 0;
    uint32 m_lastVictimGuidLow = 0;

    // --- Loot state ---
    int32  m_lootTimer = 0;           // Countdown to auto-loot after kill (humanization delay)
    ObjectGuid m_lootTargetGuid;      // GUID of creature to loot
    uint32 ChooseQuestReward(Quest const* pQuest) const; // helper for choosing quest reward

    // --- State ---
    ShortTimeTracker m_updateTimer;
    bool m_wasDead = false;
    bool m_loggedFirstUpdate = false;
    bool m_freshSpawn = false;
    uint32 m_wanderTimer = 0;
    uint32 m_lastKnownLevel = 0;
    uint32 m_trackedQuestId = 0;

    // Spawn params
    uint8 m_spawnRace;
    uint8 m_spawnClass;
    uint32 m_spawnLevel;
    uint32 m_spawnMapId;
    uint32 m_spawnInstanceId;
    uint32 CountNearbyHostiles(Unit* pCandidate, float radius) const;
    float m_spawnX, m_spawnY, m_spawnZ, m_spawnO;
    std::string m_spawnName;  // Persisted name from playerbot table (empty = generate random)

    // --- Chunked pathing state (Session 25) ---
    // Stored mmap waypoints from PathInfo::calculate().
    // MovePoint is called to waypoints[m_pathIndex] in ~200yd chunks.
    // m_pathIndex is the NEXT waypoint to walk toward.
    std::vector<Vector3> m_pathWaypoints;
    uint32 m_pathIndex = 0;

    // §4 approach-scan throttle: an enriched (objective) MOVE_TO scans for its target
    // mob every ~2-3s while walking. Reset to 0 by BridgeHandleMoveTo so the first scan
    // fires on the journey's first tick. Decremented in UpdateAI alongside m_wanderTimer.
    uint32 m_approachScanTimer = 0;


    // --- Combat stalemate breaker state ---
    // No-damage-either-way in-combat deadlock detection + escape. m_stalemateMs accumulates
    // while the deadlock holds; a nudge fires every AIBOT_STALEMATE_NUDGE_MS, and after
    // AIBOT_STALEMATE_MAX_NUDGES the bot force-disengages and ignores the guid for
    // AIBOT_STALEMATE_IGNORE_MS so Select*Target doesn't re-acquire it from the hostile-ref list.
    uint32 m_stalemateMs       = 0;   // accumulated no-damage-in-combat time
    uint32 m_stalemateHoldMs   = 0;   // mid-nudge hold (suppresses re-chase while the hop runs)
    uint8  m_stalemateNudges   = 0;   // nudges issued this stalemate
    uint8  m_stalemateDisengages = 0; // Stage-2 flee attempts before the hard-teleport escape
    uint32 m_lastHealth        = 0;   // bot hp last tick (detect damage taken)
    uint32 m_lastVictimHealth  = 0;   // victim hp last tick (detect damage dealt)
    ObjectGuid m_stalemateVictim;     // anchored stalemate target guid
    std::map<uint32, uint32> m_combatIgnore;  // victim guidLow -> ms remaining ignored

    // --- Overpull retreat state ---
    uint32 m_overpullFleeHoldMs = 0;   // mid-retreat hold (suppress re-chase while the hop runs)
    uint8  m_overpullFlees      = 0;   // retreats issued this combat (cap at AIBOT_OVERPULL_MAX_FLEES)
    uint32 m_lastAttackerCount  = 0;   // peak melee attackers this combat — stamped on the DEATH event
 // --- Grind freeze-escape (livelock fix, generalized) ---
    // Consecutive TASK_GRIND ticks that produced no pull — EITHER OverpullGuard vetoed every
    // candidate (field denser than the solo cap) OR SelectGrindTarget found nothing valid (all
    // killed/tapped/grey/red, or empty — the "nothing matches" freeze under a crowd of bots).
    // At AIBOT_GRIND_FREEZE_DWELL we emit GRIND_BLOCKED so C# breaks the freeze with a single
    // unstick kill, then re-issues the objective. Reset to 0 on any successful pull.
    uint32 m_grindFreezeStreak = 0;


    // --- Graveyard self-rez state (race-free port→rez; no C# roundtrip) ---
    // We teleport the ghost, then resurrect it OURSELVES once the teleport has actually
    // landed — so the bot can never rez back in the death pocket (the old loop). While
    // m_pendingGraveyardRez is set, inbound RESURRECT commands are ignored (a stray plain
    // rez from C# can't rez us mid-teleport). m_graveRezWaitMs is a safety timeout: if the
    // teleport never confirms, rez anyway rather than ghost-stick forever.
    bool   m_pendingGraveyardRez = false;
    float  m_graveRezX = 0.0f, m_graveRezY = 0.0f, m_graveRezZ = 0.0f;
    uint32 m_graveRezMap = 0;
    uint32 m_graveRezWaitMs = 0;

    // --- Navmesh-seam crossing state (this bot's learned cave portals) ---
    // Overworld caves whose interior mesh doesn't stitch to terrain at the portal
    // make pathing the last few yards return NOPATH forever. We teleport across on
    // entry (CheckForUnreachableTarget already does this for the combat chase case)
    // and record the seam so the exit leg can teleport back to the connected mesh.
    // Per-bot, reset on reconnect; the durable/fleet-shared version persists these.
    std::vector<NavBoundary> m_navBoundaries;   // learned at runtime
    bool m_didBoundaryExit = false;             // one outbound seam-cross per MOVE_TO journey
    bool m_didNavmeshSnap = false;   // one off-mesh-start snap attempt per MOVE_TO journey

    // --- Bridge state ---
    BridgeSocket m_bridgeSocket;
    bool m_bridgeConnected = false;
    bool m_bridgeHelloSent = false;
    uint32 m_bridgeStateTimer = 0;
    uint32 m_bridgeReconnectTimer = 0;
    uint32 m_bridgeReconnectDelay = BRIDGE_RECONNECT_BASE;
    char m_bridgeRecvBuf[BRIDGE_RECV_BUF_SIZE];
    int m_bridgeRecvLen = 0;

    // Outbound queue (Session 36). BridgeSend appends complete JSON lines here;
    // BridgeFlush drains it. Holds only whole lines, so a partial socket write
    // can never truncate a message on the wire. Per-bot, single-threaded with
    // the rest of this bot's AI — no locking needed.
    std::string m_bridgeSendBuf;
};

#endif