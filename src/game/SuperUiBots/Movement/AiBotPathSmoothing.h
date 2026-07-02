#ifndef MANGOS_AIBOT_PATHSMOOTHING_H
#define MANGOS_AIBOT_PATHSMOOTHING_H

// Pure corner-smoothing geometry for bot travel paths — no AiBotAI/navmesh dependency, trivially
// separable/testable on its own. This ONLY ever produces CANDIDATE points; it never asserts they
// are walkable or reachable. The caller (AiBotAIMovement.cpp, which owns the navmesh query +
// PathInfo reachability check + ReGroundZ) is responsible for validating every returned point
// before trusting it, falling back to a tighter technique on any rejection. See
// AiBotAI::SmoothPathCorners / AiBotAI::ValidateWideBowCandidate.
//
// TWO TECHNIQUES, composed with a fallback chain:
//
// 1. TIGHT FILLET (original) — a quadratic Bezier at a single sharp vertex B, steered between B
//    itself and the Entry/Exit midpoint. Every point is a convex combination of Entry/B/Exit, so
//    it's mathematically bounded inside that small triangle: can round a corner, can never swing
//    wider than B (the tightest legal point Detour already validated) or cross the straight A-C
//    line. Safe by construction; used as the fallback when a wide bow doesn't apply/validate.
//
// 2. WIDE BOW — reaches back along the straight approach BEFORE the corner and forward past it,
//    then curves the whole leg outward through open ground rather than hugging the vertex. This is
//    what lets a bot cut a visible diagonal across a plaza instead of beelining to the wall and
//    pivoting — a tight fillet geometrically cannot do this, by design (see above). A wide bow has
//    NO geometric safety bound the way a fillet does — it reaches into terrain the original path
//    never touched — so validation (on-mesh check + a real PathInfo reachability proof between
//    every consecutive sample) carries the entire safety burden.
//
//    CHORD/WIDTH PROPORTIONALITY (2026-07-01, fixes an observed "bot loops toward target" bug).
//    RollWideBowPlan's width roll is independent of how much anchor reach a corner actually gets —
//    and on a corner-dense path (confirmed live: 7-12 corner-runs on a 150-300yd path, i.e. a real
//    corner roughly every 20-40yd), neighboring corners' anchor windows collide constantly and get
//    clamped down by SmoothPathCorners so they don't overlap. A corner clamped to a short usable
//    chord can still roll a wide offset — e.g. a 15yd-wide bow over an 8yd forward chord isn't a
//    sweep, it's a hairpin, and strung across a dozen close corners each independently rolling its
//    own side, that reads as the bot looping rather than progressing. The tight fillet already
//    guards against exactly this shape of bug (pullback capped at 40% of the adjacent segment); the
//    wide bow never got the equivalent guard until now. kBowMinChordYards / kBowWidthToChordRatio
//    are that guard — applied by the CALLER (SmoothPathCorners) once the real, clamped anchor
//    distance is known, since only the caller knows what clamping actually happened. See the
//    caller-side comment in AiBotAIMovement.cpp for exactly where.
//
// RANDOMIZATION SEED, both techniques: journeySeed (AiBotAI::m_pathJourneySeed) + the corner's own
// location — NOT the bot's permanent identity. Re-rolls only when dispatched to a genuinely new
// destination; holds steady across internal recursion and rapid same-leg re-issues, so a bot mid-
// turn on one trip never visibly snaps to a different curve partway through, but a bot revisiting
// the same corner on a later, unrelated errand rounds it differently than it did last time.

#include "PathFinder.h"   // Vector3, PointsArray
#include "Common.h"       // uint32

namespace AiBotPathSmoothing
{
    // ── Tight fillet tuning ──
    float const kFilletAngleThresholdDeg = 30.0f;   // merged-run cumulative turn beyond this counts as "a corner"
    float const kFilletMaxPullbackYards  = 2.5f;
    float const kFilletMinPullbackYards  = 0.5f;
    float const kFilletMaxCutFraction    = 0.55f;
    uint32 const kFilletInteriorPoints   = 2;

    // ── Wide-bow tuning ──
    float const kBowAnchorMinYards = 15.0f;   // how far back/forward from the corner the bow's anchors sit, roll range min
    float const kBowAnchorMaxYards = 35.0f;   // roll range max
    float const kBowMinOffsetYards = 3.0f;    // width roll floor — even a "small" bow is still perceptible
    float const kBowMaxOffsetYards = 20.0f;   // width roll ceiling — a real diagonal-cut sweep, not absurd
    uint32 const kBowSamplePoints  = 4;        // interior curve samples (excl. anchors)

    // Chord/width proportionality guard (2026-07-01) — see the header note above for why this
    // exists. Applied by the CALLER against the REAL post-clamp anchor distance, not here.
    float const kBowMinChordYards     = 15.0f;   // below this actual anchor-to-anchor distance, don't attempt a bow at all — too little room for a sensible sweep, fall straight to the tight fillet.
    float const kBowWidthToChordRatio = 0.30f;   // hard cap: effective width can never exceed this fraction of the REAL (possibly-clamped) chord, regardless of what RollWideBowPlan rolled. The wide-bow analog of the fillet's 40%-of-segment pullback clamp.

    // Computes the candidate tight-fillet replacement for vertex B given neighbors A/C, randomized
    // off journeySeed + B's location. False = turn too gentle or segments too short; caller keeps
    // the original point(s). True = outFillet is Entry, interior(s)..., Exit, always >= 3 points.
    bool ComputeCornerFillet(Vector3 const& A, Vector3 const& B, Vector3 const& C, uint32 journeySeed, PointsArray& outFillet);

    // A corner's rolled wide-bow parameters — side/width/anchor-reach-distances, all deterministic
    // off journeySeed + the corner's own peak point (see AiBotAI::SmoothPathCorners for how "peak"
    // is chosen when a corner spans several resampled Detour points). NOTE: widthYards here is the
    // UNCLAMPED roll — the caller must still apply kBowMinChordYards/kBowWidthToChordRatio against
    // the real anchor distance before using it (see the header note above).
    struct WideBowPlan
    {
        int preferredSide;      // +1 or -1 — try this side first, the other on rejection
        float widthYards;       // squared-roll biased toward kBowMinOffsetYards, see .cpp — UNCLAMPED against chord, caller must clamp
        float anchorBackYards;  // desired reach back along the approach (actual reach may clamp shorter — see WalkBackwardForDistance)
        float anchorFwdYards;   // desired reach forward past the corner
    };
    WideBowPlan RollWideBowPlan(Vector3 const& cornerPeak, uint32 journeySeed);

    // Produces sample points for a bow between anchorStart and anchorEnd, offset perpendicular by
    // widthYards on the given side. Raised-cosine profile — eases the offset to exactly zero WITH
    // zero derivative at both ends, so the bow joins the straight path tangent-continuously (no kink
    // at the splice, unlike the tight fillet's small triangle). outBow does NOT include
    // anchorStart/anchorEnd — caller splices them. Empty output = degenerate (anchors too close).
    // widthYards is taken as-is — this function does NOT apply the chord/width proportionality
    // guard; the caller is responsible for passing an already-clamped effective width.
    void ComputeWideBowSamples(Vector3 const& anchorStart, Vector3 const& anchorEnd, float widthYards, int side, PointsArray& outBow);

    // Walk backward/forward along `path` from fromIdx, accumulating real segment distance, until
    // targetDist is reached OR minIdx/maxIdx is hit (whichever first — a short adjacent straight run
    // clamps the reach rather than failing). Pure array math, no AiBotAI/navmesh dependency.
    uint32 WalkBackwardForDistance(PointsArray const& path, uint32 fromIdx, float targetDist, uint32 minIdx);
    uint32 WalkForwardForDistance(PointsArray const& path, uint32 fromIdx, float targetDist, uint32 maxIdx);
}

#endif