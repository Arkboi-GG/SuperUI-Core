#ifndef MANGOS_AIBOT_MOVEMENTGENERATORS_H
#define MANGOS_AIBOT_MOVEMENTGENERATORS_H

// Bot-scoped movement generator(s) + the one friended issuer that can push them onto a
// MotionMaster. Kept entirely out of the core PointMovementGenerator.h/.cpp and MotionMaster.h/.cpp
// bodies — the only core touch this depends on is the single `friend class AiBotMovementIssuer;`
// line in MotionMaster.h.

#include "PointMovementGenerator.h"
#include "PathFinder.h"   // PointsArray
#include "Player.h"

// Point movement realized by feeding a caller-supplied point sequence directly to the spline
// (Movement::MoveSplineInit::MovebyPath), instead of MoveTo's single-destination internal
// re-pathfind (MOVE_PATHFINDING). Same lifecycle/notify contract as PointMovementGenerator<Player>
// — GetMovementGeneratorType() stays POINT_MOTION_TYPE, same MovementInform(id) on arrival — so any
// caller that only knows the base type sees no difference except a smoother spline.
//
// LANDMINE (read before touching): on the base class, Interrupt/Reset/Update are NOT virtual —
// only Initialize/Finalize/MovementInform are. A further override of Interrupt/Reset/Update here
// would silently never be called via the CRTP dispatch in MovementGeneratorMedium — don't add one.
class AiBotSmoothedPointMovementGenerator : public PointMovementGenerator<Player>
{
    public:
        AiBotSmoothedPointMovementGenerator(uint32 id, PointsArray const& path, uint32 options,
            float speed = 0.0f, float finalOrientation = -10.0f) :
          PointMovementGenerator<Player>(id,
              path.empty() ? 0.0f : path.back().x,
              path.empty() ? 0.0f : path.back().y,
              path.empty() ? 0.0f : path.back().z,
              options, speed, finalOrientation),
          m_path(path)
        {}

        void Initialize(Player& unit) override;

    private:
        PointsArray m_path;
};

// The one class friended in MotionMaster.h. Its only job is being the sanctioned caller of the
// private Mutate() — no other logic belongs here; smoothing/fillet math stays in AiBotAIMovement.cpp.
class AiBotMovementIssuer
{
    public:
        static void IssueSmoothedPath(Player& bot, uint32 id, PointsArray const& path,
            uint32 options = MOVE_NONE, float speed = 0.0f, float finalOrientation = -10.0f);
};

#endif