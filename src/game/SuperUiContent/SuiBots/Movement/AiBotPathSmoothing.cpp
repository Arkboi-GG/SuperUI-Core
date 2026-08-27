#include "AiBotPathSmoothing.h"
#include <cmath>

namespace AiBotPathSmoothing
{
    static Vector3 Lerp3(Vector3 const& a, Vector3 const& b, float t)
    {
        Vector3 out;
        out.x = a.x + (b.x - a.x) * t;
        out.y = a.y + (b.y - a.y) * t;
        out.z = a.z + (b.z - a.z) * t;
        return out;
    }

    static Vector3 QuadraticBezier(Vector3 const& p0, Vector3 const& p1, Vector3 const& p2, float t)
    {
        float const u = 1.0f - t;
        float const w0 = u * u;
        float const w1 = 2.0f * t * u;
        float const w2 = t * t;
        Vector3 out;
        out.x = p0.x * w0 + p1.x * w1 + p2.x * w2;
        out.y = p0.y * w0 + p1.y * w1 + p2.y * w2;
        out.z = p0.z * w0 + p1.z * w1 + p2.z * w2;
        return out;
    }

    // Cheap integer hash (splitmix32-style finalizer) — good avalanche, not cryptographic.
    static float DeterministicUnitFloat(uint32 seed)
    {
        seed ^= seed >> 16;
        seed *= 0x7feb352dU;
        seed ^= seed >> 15;
        seed *= 0x846ca68bU;
        seed ^= seed >> 16;
        return (float)(seed & 0xFFFFFFu) / (float)0x1000000u;   // 24 bits -> [0,1)
    }

    static uint32 CornerSeed(uint32 journeySeed, Vector3 const& B)
    {
        int32 const qx = (int32)lroundf(B.x);
        int32 const qy = (int32)lroundf(B.y);
        uint32 h = journeySeed;
        h = h * 2654435761u + (uint32)qx;
        h = h * 2654435761u + (uint32)qy;
        return h;
    }

    bool ComputeCornerFillet(Vector3 const& A, Vector3 const& B, Vector3 const& C, uint32 journeySeed, PointsArray& outFillet)
    {
        outFillet.clear();

        float const inX = B.x - A.x, inY = B.y - A.y;
        float const outX = C.x - B.x, outY = C.y - B.y;

        float const segLenIn = sqrtf(inX * inX + inY * inY);
        float const segLenOut = sqrtf(outX * outX + outY * outY);
        if (segLenIn < 0.01f || segLenOut < 0.01f)
            return false;   // cb:fold pure geometry, no bot context

        float const dot = (inX * outX + inY * outY) / (segLenIn * segLenOut);
        float const cosTheta = dot > 1.0f ? 1.0f : (dot < -1.0f ? -1.0f : dot);
        float const turnDeg = acosf(cosTheta) * (180.0f / 3.14159265f);

        if (turnDeg < kFilletAngleThresholdDeg)
            return false;   // cb:fold pure geometry, no bot context

        uint32 const seed = CornerSeed(journeySeed, B);
        float const rollPullback = DeterministicUnitFloat(seed);
        float const rollCut      = DeterministicUnitFloat(seed ^ 0x9e3779b9u);

        float pullback = kFilletMinPullbackYards + rollPullback * (kFilletMaxPullbackYards - kFilletMinPullbackYards);
        if (pullback > segLenIn * 0.4f)  pullback = segLenIn * 0.4f;   // cb:fold pure geometry, no bot context
        if (pullback > segLenOut * 0.4f) pullback = segLenOut * 0.4f;   // cb:fold pure geometry, no bot context
        if (pullback < kFilletMinPullbackYards)
            return false;   // cb:fold pure geometry, no bot context

        Vector3 const entry = Lerp3(B, A, pullback / segLenIn);
        Vector3 const exit  = Lerp3(B, C, pullback / segLenOut);

        Vector3 const midpoint = Lerp3(entry, exit, 0.5f);
        float const cutAmount = rollCut * kFilletMaxCutFraction;
        Vector3 const control = Lerp3(B, midpoint, cutAmount);

        outFillet.push_back(entry);
        for (uint32 i = 1; i <= kFilletInteriorPoints; ++i)
        {
            float const t = (float)i / (float)(kFilletInteriorPoints + 1);
            outFillet.push_back(QuadraticBezier(entry, control, exit, t));
        }
        outFillet.push_back(exit);

        return true;
    }

    WideBowPlan RollWideBowPlan(Vector3 const& cornerPeak, uint32 journeySeed)
    {
        int32 const qx = (int32)lroundf(cornerPeak.x);
        int32 const qy = (int32)lroundf(cornerPeak.y);
        uint32 h = journeySeed ^ 0xB0A7C0DEu;   // distinguishing constant — decorrelates the bow's
                                                  // roll from ComputeCornerFillet's own roll on the
                                                  // same peak point, so "does this corner bow" and
                                                  // "how does its fillet fallback look" don't secretly track each other
        h = h * 2654435761u + (uint32)qx;
        h = h * 2654435761u + (uint32)qy;

        float const rollSide    = DeterministicUnitFloat(h);
        float const rollWidth   = DeterministicUnitFloat(h ^ 0x9e3779b9u);
        float const rollAnchorA = DeterministicUnitFloat(h ^ 0x1234abcdu);
        float const rollAnchorB = DeterministicUnitFloat(h ^ 0x87654321u);

        WideBowPlan plan;
        plan.preferredSide = rollSide < 0.5f ? -1 : 1;

        // Squared roll — biases toward SMALLER widths on average (mean ~1/3 of the range vs 1/2
        // for a uniform roll), per Nico's ask: most corners get a modest sweep, a wide diagonal cut
        // is the less-common case, not the default.
        float const biased = rollWidth * rollWidth;
        plan.widthYards = kBowMinOffsetYards + biased * (kBowMaxOffsetYards - kBowMinOffsetYards);

        plan.anchorBackYards = kBowAnchorMinYards + rollAnchorA * (kBowAnchorMaxYards - kBowAnchorMinYards);
        plan.anchorFwdYards  = kBowAnchorMinYards + rollAnchorB * (kBowAnchorMaxYards - kBowAnchorMinYards);
        return plan;
    }

    void ComputeWideBowSamples(Vector3 const& anchorStart, Vector3 const& anchorEnd, float widthYards, int side, PointsArray& outBow)
    {
        outBow.clear();

        float const dx = anchorEnd.x - anchorStart.x;
        float const dy = anchorEnd.y - anchorStart.y;
        float const chordLen = sqrtf(dx * dx + dy * dy);
        if (chordLen < 1.0f)
            return;   // anchors too close — nothing sane to bow, caller falls back to the fillet   // cb:fold pure geometry, no bot context

        float const perpX = -(dy / chordLen) * (float)side;
        float const perpY =  (dx / chordLen) * (float)side;

        for (uint32 i = 1; i <= kBowSamplePoints; ++i)
        {
            float const t = (float)i / (float)(kBowSamplePoints + 1);

            // Raised-cosine bump: 0 at t=0/t=1 with ZERO derivative at both ends (tangent-continuous
            // join — unlike the tight fillet's small abrupt-entry triangle), peaks at widthYards at t=0.5.
            float const bump = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * t));
            float const offset = widthYards * bump;

            Vector3 const base = Lerp3(anchorStart, anchorEnd, t);
            Vector3 p;
            p.x = base.x + perpX * offset;
            p.y = base.y + perpY * offset;
            p.z = base.z;   // caller resolves real Z via ReGroundZ during validation
            outBow.push_back(p);
        }
    }

    uint32 WalkBackwardForDistance(PointsArray const& path, uint32 fromIdx, float targetDist, uint32 minIdx)
    {
        float accumulated = 0.0f;
        uint32 idx = fromIdx;
        while (idx > minIdx)
        {
            float const dx = path[idx].x - path[idx - 1].x;
            float const dy = path[idx].y - path[idx - 1].y;
            accumulated += sqrtf(dx * dx + dy * dy);
            --idx;
            if (accumulated >= targetDist)
                break;   // cb:fold pure geometry, no bot context
        }
        return idx;
    }

    uint32 WalkForwardForDistance(PointsArray const& path, uint32 fromIdx, float targetDist, uint32 maxIdx)
    {
        float accumulated = 0.0f;
        uint32 idx = fromIdx;
        while (idx < maxIdx)
        {
            float const dx = path[idx + 1].x - path[idx].x;
            float const dy = path[idx + 1].y - path[idx].y;
            accumulated += sqrtf(dx * dx + dy * dy);
            ++idx;
            if (accumulated >= targetDist)
                break;   // cb:fold pure geometry, no bot context
        }
        return idx;
    }
}