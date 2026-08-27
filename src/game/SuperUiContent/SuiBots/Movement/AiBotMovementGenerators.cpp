#include "AiBotMovementGenerators.h"
#include "../AiBotCircuit.h" // [CIRCUIT] probe macros (CIRCUIT_BOARD.md)
#include "MoveSplineInit.h"
#include "MotionMaster.h"

void AiBotSmoothedPointMovementGenerator::Initialize(Player& unit)
{
    if (!unit.IsStopped())
    {
        CB_HIT(unit.GetGUIDLow(), "cpp-move: stopping prior motion for smoothed spline");
        unit.StopMoving();
    }

    unit.AddUnitState(UNIT_STATE_ROAMING | UNIT_STATE_ROAMING_MOVE);
    Movement::MoveSplineInit init(unit, "AiBotSmoothedPointMovementGenerator::Initialize");
    init.MovebyPath(m_path, 0);
    if (m_speed > 0.0f)
        init.SetVelocity(m_speed);   // cb:fold spline option application detail
    if (m_options & MOVE_WALK_MODE)
        init.SetWalk(true);   // cb:fold spline option application detail
    if (m_options & MOVE_RUN_MODE)
        init.SetWalk(false);   // cb:fold spline option application detail
    if (m_options & MOVE_FLY_MODE)
        init.SetFly();   // cb:fold spline option application detail
    if (m_options & MOVE_FALLING)
        init.SetFall();   // cb:fold spline option application detail
    if (m_options & MOVE_CYCLIC)
        init.SetCyclic();   // cb:fold spline option application detail
    if (m_o > -7.0f)
        init.SetFacing(m_o);   // cb:fold spline option application detail
    init.Launch();
}

void AiBotMovementIssuer::IssueSmoothedPath(Player& bot, uint32 id, PointsArray const& path,
    uint32 options, float speed, float finalOrientation)
{
    if (path.empty())
    {
        CB_HIT(bot.GetGUIDLow(), "cpp-move: smoothed path empty, not issuing");
        return;
    }

    bot.GetMotionMaster()->Mutate(new AiBotSmoothedPointMovementGenerator(id, path, options, speed, finalOrientation));
}