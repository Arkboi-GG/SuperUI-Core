/*
 * RaidPlanLaw.cpp — parse + formation math for the raid plan (PLAN_19 M-C).
 *
 * Self-contained on purpose: its own minimal flat-JSON extractors (the same naive
 * house idiom AiBotAIBridge.cpp uses file-locally) and no game includes, so the
 * whole TU can be exercised by a plain unit main. The bridge handler in
 * AiBotAIBridge.cpp is a thin adopt-and-ack shell around SuiParseRaidPlan.
 *
 * Wire shape (one flat JSON line; lists are the house pipe strings):
 *   {"type":"LOAD_RAID_PLAN","schema":1,"plan":"...","encounter":"onyxia",
 *    "d_formation":1,"d_dodge":1,"d_cones":1,"d_spread":1,"d_spreadyd":8.0,
 *    "d_groupheal":1,"d_threatlite":0,
 *    "assignments":"p2:1:2:0|p3:1:2:0",       // phase:job:fromOrdinal:target
 *    "auras":"6346:5:180000:30000:1",         // spell:class:durMs:cdMs:tankOrd
 *    "addctl":"8:8.0:0.5:3:30.0",             // class:radius:slow:minAdds:range
 *    "b_job":1,"b_side":1,"b_class":1,"b_rot":"rot-1",
 *    "b_avoid":"deep_breath,fireball",
 *    "b_targets":"p2:0|p3:2,0"}               // phase:targetCSV (ordered)
 *
 * Line endings: LF (C++ repo convention).
 */

#include "RaidPlanLaw.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// ============================================================
// Minimal flat-JSON extractors (file-local, same contract as the
// bridge's own: naive, flat keys only, good enough for machine-
// written single-line payloads).
// ============================================================

static char const* FindKey(char const* json, char const* key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    char const* at = strstr(json, pattern);
    return at ? at + strlen(pattern) : nullptr;
}

static bool ExtractString(char const* json, char const* key, char* out, size_t outLen)
{
    out[0] = '\0';
    char const* at = FindKey(json, key);
    if (!at || *at != '"') return false;
    ++at;
    size_t i = 0;
    while (*at && *at != '"' && i + 1 < outLen)
        out[i++] = *at++;
    out[i] = '\0';
    return true;
}

static bool ExtractInt(char const* json, char const* key, int& out)
{
    char const* at = FindKey(json, key);
    if (!at) return false;
    return sscanf(at, "%d", &out) == 1;
}

static bool ExtractFloat(char const* json, char const* key, float& out)
{
    char const* at = FindKey(json, key);
    if (!at) return false;
    return sscanf(at, "%f", &out) == 1;
}

static bool ExtractBool(char const* json, char const* key, bool& out, bool fallback)
{
    int v;
    out = ExtractInt(json, key, v) ? v != 0 : fallback;
    return true;
}

// Split a pipe list into segments and hand each to `fn`; count outcomes.
template <typename Fn>
static void ForEachPipeSegment(char* buf, Fn fn)
{
    char* save = nullptr;
    for (char* seg = strtok_r(buf, "|", &save); seg; seg = strtok_r(nullptr, "|", &save))
        fn(seg);
}

// ============================================================
// SuiParseRaidPlan — validate-before-adopt.
// ============================================================

bool SuiParseRaidPlan(char const* json, SuiRaidPlan& out, SuiRaidPlanDiag& diag,
                      char* err, size_t errLen)
{
    SuiRaidPlan plan;
    diag = SuiRaidPlanDiag();

    if (!ExtractInt(json, "schema", plan.schema) || plan.schema != 1)
    {
        snprintf(err, errLen, "schema missing or unsupported (want 1)");
        return false;
    }

    char buf[256];
    if (ExtractString(json, "plan", buf, sizeof(buf))) plan.name = buf;
    if (ExtractString(json, "encounter", buf, sizeof(buf))) plan.encounterKey = buf;

    SuiRaidDoctrine& d = plan.doctrine;
    ExtractBool(json, "d_formation", d.deriveFormation, true);
    ExtractBool(json, "d_dodge", d.dodgeTelegraphs, true);
    ExtractBool(json, "d_cones", d.keepClearOfCones, true);
    ExtractBool(json, "d_spread", d.spreadFromTargeted, true);
    ExtractFloat(json, "d_spreadyd", d.spreadYards);
    ExtractBool(json, "d_groupheal", d.groupHealing, true);
    ExtractBool(json, "d_threatlite", d.bossThreatLite, false);
    if (d.spreadYards <= 0.f || d.spreadYards > 60.f) d.spreadYards = 8.f;

    char listBuf[2048];
    if (ExtractString(json, "assignments", listBuf, sizeof(listBuf)) && listBuf[0])
        ForEachPipeSegment(listBuf, [&](char* seg)
        {
            char phase[48]; int job, ord, target;
            if (sscanf(seg, "%47[^:]:%d:%d:%d", phase, &job, &ord, &target) != 4 ||
                job < 0 || job > 4 || target < 0 || target > 2 || ord < 1)
            { ++diag.skipped; return; }
            SuiPhaseJobAssignment a;
            a.phaseKey = phase; a.job = (uint8_t)job;
            a.fromOrdinal = ord; a.target = (uint8_t)target;
            d.assignments.push_back(a);
            ++diag.assignments;
        });

    if (ExtractString(json, "auras", listBuf, sizeof(listBuf)) && listBuf[0])
        ForEachPipeSegment(listBuf, [&](char* seg)
        {
            unsigned spell, cls; int dur, cd, ord;
            if (sscanf(seg, "%u:%u:%d:%d:%d", &spell, &cls, &dur, &cd, &ord) != 5 ||
                spell == 0 || cls == 0 || dur <= 0 || ord < 1)
            { ++diag.skipped; return; }
            SuiMaintainAuraRule r;
            r.spellId = spell; r.casterClassId = cls;
            r.durationMs = dur; r.cooldownMs = cd < 0 ? 0 : cd;
            r.targetTankOrdinal = ord;
            d.maintainAuras.push_back(r);
            ++diag.auras;
        });

    if (ExtractString(json, "addctl", listBuf, sizeof(listBuf)) && listBuf[0])
        ForEachPipeSegment(listBuf, [&](char* seg)
        {
            unsigned cls; float radius, slow, range; int minAdds;
            if (sscanf(seg, "%u:%f:%f:%d:%f", &cls, &radius, &slow, &minAdds, &range) != 5 ||
                cls == 0 || radius <= 0.f || slow <= 0.f || slow > 1.f || minAdds < 1)
            { ++diag.skipped; return; }
            SuiAddControlJob j;
            j.casterClassId = cls; j.radiusYards = radius; j.slowFactor = slow;
            j.minAdds = minAdds; j.castRangeYards = range > 0.f ? range : 30.f;
            d.addControl.push_back(j);
            ++diag.addControl;
        });

    // ── this bot's slice ──
    int v;
    if (ExtractInt(json, "b_job", v) && v >= 0 && v <= 4) plan.job = (uint8_t)v;
    if (ExtractInt(json, "b_side", v) && v >= 0 && v <= 3) plan.side = (uint8_t)v;
    if (ExtractInt(json, "b_class", v) && v >= 0) plan.classId = (uint32_t)v;
    if (ExtractString(json, "b_rot", buf, sizeof(buf))) plan.rotationId = buf;
    if (ExtractInt(json, "b_slot", v) && v >= 0) plan.slotIndex = v;
    if (ExtractInt(json, "b_slotcount", v) && v >= 1) plan.slotCount = v;
    if (ExtractInt(json, "b_mt", v)) plan.mainTank = v != 0 ? 1 : 0;

    if (ExtractString(json, "b_avoid", listBuf, sizeof(listBuf)) && listBuf[0])
    {
        char* save = nullptr;
        for (char* key = strtok_r(listBuf, ",", &save); key; key = strtok_r(nullptr, ",", &save))
            if (*key) { plan.avoidAbilityKeys.emplace_back(key); ++diag.avoids; }
    }

    if (ExtractString(json, "b_targets", listBuf, sizeof(listBuf)) && listBuf[0])
        ForEachPipeSegment(listBuf, [&](char* seg)
        {
            char* colon = strchr(seg, ':');
            if (!colon || colon == seg) { ++diag.skipped; return; }
            SuiPhaseTargets pt;
            pt.phaseKey.assign(seg, colon - seg);
            char* save = nullptr;
            for (char* k = strtok_r(colon + 1, ",", &save); k; k = strtok_r(nullptr, ",", &save))
            {
                int kind;
                if (sscanf(k, "%d", &kind) == 1 && kind >= 0 && kind <= 2)
                    pt.order.push_back((uint8_t)kind);
            }
            if (pt.order.empty()) { ++diag.skipped; return; }
            plan.phaseTargets.push_back(pt);
            ++diag.phaseTargets;
        });

    out = plan;   // adopt only now: every earlier return left the caller's plan alone
    err[0] = '\0';
    return true;
}

// ============================================================
// SuiFormationLaw — the client's RaidFormationLaw, verbatim.
// ============================================================

namespace SuiFormationLaw
{
    static constexpr float Pi = 3.14159265358979f;
    static constexpr float Tau = 2.f * Pi;

    void SafeBand(float frontHalfRad, float rearHalfRad, float& fromRad, float& toRad)
    {
        fromRad = frontHalfRad + ArcMarginRad;
        toRad = Pi - rearHalfRad - ArcMarginRad;
        if (toRad <= fromRad)
        {
            fromRad = Pi / 2.f;
            toRad = Pi / 2.f;
        }
    }

    float SlotAngle(float fromRad, float toRad, int index, int count)
    {
        int n = count > 1 ? count : 1;
        return fromRad + (toRad - fromRad) * ((index + 0.5f) / (float)n);
    }

    void Station(float bossX, float bossY, float bossFacing, float angleOffNose,
                 int sideSign, float radius, float& outX, float& outY)
    {
        float bearing = bossFacing + (float)sideSign * angleOffNose;
        outX = bossX + std::cos(bearing) * radius;
        outY = bossY + std::sin(bearing) * radius;
    }

    void AirStation(float bossX, float bossY, int index, int count, float radius,
                    float& outX, float& outY)
    {
        int n = count > 1 ? count : 1;
        float bearing = Tau * (float)index / (float)n;
        outX = bossX + std::cos(bearing) * radius;
        outY = bossY + std::sin(bearing) * radius;
    }

    float RangedRadius(float coneRangeYards, float meleeReach)
    {
        float r = coneRangeYards + 4.f;
        float floorR = meleeReach + 15.f;
        if (r < floorR) r = floorR;
        if (r < 12.f) r = 12.f;
        if (r > 30.f) r = 30.f;
        return r;
    }

    float HealerRadius(float coneRangeYards, float meleeReach)
    {
        return (meleeReach + RangedRadius(coneRangeYards, meleeReach)) * .5f;
    }
}
