#ifndef MANGOS_RAIDPLANLAW_H
#define MANGOS_RAIDPLANLAW_H

// =====================================================================================
//  RaidPlanLaw.h — the raid plan: typed doctrine + the pure formation law.
//  PLAN_19 M-C (MSUIClient docs/plans/PLAN_19_RAID_DOCTRINE_PIPELINE.md).
//
//  Two halves, deliberately dependency-free (no Unit/Player/World includes) so the
//  TU is unit-testable exactly like the client's law:
//
//    1. SuiRaidPlan — the parsed LOAD_RAID_PLAN payload: the raid-wide doctrine
//       (formation / dodging / spread / healing switches, bucket assignments,
//       maintain-aura chains, add-control jobs, opt-in boss threat-lite) plus THIS
//       bot's slice (job, macro group, class, rotation ref, avoid keys, per-phase
//       target orders). Parsed with validate-before-adopt: a malformed payload
//       refuses loudly and the previous plan stands.
//
//    2. SuiFormationLaw — the formation math, ported VERBATIM from the client's
//       RaidFormationLaw (MSUIClient/World/Encounters/RaidDoctrine.cs). Same
//       constants, same formulas: the client's encounter-lab-check fixtures are
//       the behavioural contract for both executors. Change one side only with a
//       matching change and test on the other.
//
//  Enum encodings on the wire (match the client enums by ordinal):
//    job:    0 None, 1 Tank, 2 Healer, 3 Melee, 4 Ranged        (RaidJob)
//    side:   0 None/auto, 1 Left(G1), 2 Center, 3 Right(G2)     (RaidSide)
//    target: 0 AnyAdd, 1 CurrentEnemy, 2 PrimaryEnemy           (CombatEnemyKind)
//
//  Line endings: LF (C++ repo convention).
// =====================================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SuiPhaseJobAssignment
{
    std::string phaseKey;
    uint8_t job = 0;
    int fromOrdinal = 1;
    uint8_t target = 2;
};

struct SuiMaintainAuraRule
{
    uint32_t spellId = 0;
    uint32_t casterClassId = 0;
    int durationMs = 0;
    int cooldownMs = 0;
    int targetTankOrdinal = 1;
};

struct SuiAddControlJob
{
    uint32_t casterClassId = 0;
    float radiusYards = 8.f;
    float slowFactor = 0.5f;
    int minAdds = 3;
    float castRangeYards = 30.f;
};

struct SuiPhaseTargets
{
    std::string phaseKey;
    std::vector<uint8_t> order;   // CombatEnemyKind ordinals, first-match-wins
};

struct SuiRaidDoctrine
{
    bool deriveFormation = true;
    bool dodgeTelegraphs = true;
    bool keepClearOfCones = true;
    bool spreadFromTargeted = true;
    float spreadYards = 8.f;
    bool groupHealing = true;
    bool bossThreatLite = false;
    std::vector<SuiPhaseJobAssignment> assignments;
    std::vector<SuiMaintainAuraRule> maintainAuras;
    std::vector<SuiAddControlJob> addControl;
};

struct SuiRaidPlan
{
    int schema = 0;
    std::string name;
    std::string encounterKey;
    SuiRaidDoctrine doctrine;
    // this bot's slice
    uint8_t job = 0;
    uint8_t side = 0;
    uint32_t classId = 0;
    std::string rotationId;
    std::vector<std::string> avoidAbilityKeys;
    std::vector<SuiPhaseTargets> phaseTargets;

    // Formation meta, PRE-RESOLVED by the web push (one bot only knows itself;
    // the pusher knows the whole roster): slot within (bucket, side), and the
    // main-tank flag. Defaults keep an old payload standing mid-band alone.
    int slotIndex = 0;
    int slotCount = 1;
    int mainTank = 0;
};

/// Per-section diagnostics for the ack: how much loaded, how much was refused.
struct SuiRaidPlanDiag
{
    int assignments = 0;
    int auras = 0;
    int addControl = 0;
    int phaseTargets = 0;
    int avoids = 0;
    int skipped = 0;   // malformed records dropped (reported, never guessed at)
};

/// Parse a LOAD_RAID_PLAN payload into `out`. Validate-before-adopt: on false the
/// caller must keep its previous plan; `err` carries the reason. Malformed list
/// records are skipped and counted in `diag.skipped` — per-rule honesty, not a
/// silent shrug.
bool SuiParseRaidPlan(char const* json, SuiRaidPlan& out, SuiRaidPlanDiag& diag,
                      char* err, size_t errLen);

// -------------------------------------------------------------------------------------
//  The formation law — ported from the client's RaidFormationLaw, function for
//  function. All pure; positions are (x, y) pairs in world space, angles radians.
// -------------------------------------------------------------------------------------
namespace SuiFormationLaw
{
    /// Angular safety margin beyond a cone's own half-arc (~12°).
    constexpr float ArcMarginRad = 0.21f;

    /// Where the safe flank band begins/ends, as absolute angles off the boss's
    /// nose. Collapses to a beam at 90° when the margins overlap.
    void SafeBand(float frontHalfRad, float rearHalfRad, float& fromRad, float& toRad);

    /// A slot's angle inside the band: bodies fan evenly, never on the edges.
    float SlotAngle(float fromRad, float toRad, int index, int count);

    /// The station: boss-relative polar. sideSign +1 = boss's LEFT (facing + angle,
    /// Group 1), -1 = her right (Group 2).
    void Station(float bossX, float bossY, float bossFacing, float angleOffNose,
                 int sideSign, float radius, float& outX, float& outY);

    /// The air-phase ring: even spread on a circle.
    void AirStation(float bossX, float bossY, int index, int count, float radius,
                    float& outX, float& outY);

    /// Ranged standoff: past the longest instant cone with margin, floored at bow
    /// range, capped inside 1.12's 30-yd envelope.
    float RangedRadius(float coneRangeYards, float meleeReach);

    /// Healers sit between the melee ball and the ranged line.
    float HealerRadius(float coneRangeYards, float meleeReach);
}

#endif
