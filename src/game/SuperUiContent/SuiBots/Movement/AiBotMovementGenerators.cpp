#include "AiBotMovementGenerators.h"
#include "MoveSplineInit.h"
#include "MotionMaster.h"

void AiBotSmoothedPointMovementGenerator::Initialize(Player& unit)
{
    if (!unit.IsStopped())
        unit.StopMoving();

    unit.AddUnitState(UNIT_STATE_ROAMING | UNIT_STATE_ROAMING_MOVE);
    Movement::MoveSplineInit init(unit, "AiBotSmoothedPointMovementGenerator::Initialize");
    init.MovebyPath(m_path, 0);
    if (m_speed > 0.0f)
        init.SetVelocity(m_speed);
    if (m_options & MOVE_WALK_MODE)
        init.SetWalk(true);
    if (m_options & MOVE_RUN_MODE)
        init.SetWalk(false);
    if (m_options & MOVE_FLY_MODE)
        init.SetFly();
    if (m_options & MOVE_FALLING)
        init.SetFall();
    if (m_options & MOVE_CYCLIC)
        init.SetCyclic();
    if (m_o > -7.0f)
        init.SetFacing(m_o);
    init.Launch();
}

void AiBotMovementIssuer::IssueSmoothedPath(Player& bot, uint32 id, PointsArray const& path,
    uint32 options, float speed, float finalOrientation)
{
    if (path.empty())
        return;

    bot.GetMotionMaster()->Mutate(new AiBotSmoothedPointMovementGenerator(id, path, options, speed, finalOrientation));
}