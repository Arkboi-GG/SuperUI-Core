/*
 * SuperUI party flight (owner decision 2026-09-03).
 *
 * Command View only: the commander clicks a destination on the flight-master
 * map and the WHOLE party they command takes that flight — their own
 * character plus every group member that is a bot for them
 * (SuiCompanion::MayCommand). Members who cannot board (node not discovered,
 * no coin, too far from the flight master, busy, already flying, other map)
 * are reported back; unless the request carries FLAG_CONFIRMED nobody flies
 * and the client asks "fly with the rest?". A confirmed request flies the
 * eligible members and reports who stayed behind.
 *
 * Direct control is deliberately NOT this wire: while possessing, the stock
 * taxi handlers run as the driven bot (GetSuiActor) and the human rides along;
 * the rest of the party holds where it stands (DoPartyFollow's in-flight hold).
 */

#ifndef MANGOS_SUI_TAXI_H
#define MANGOS_SUI_TAXI_H

#include "Common.h"
#include "ObjectGuid.h"

#include <vector>

class WorldSession;

namespace SuiTaxi
{
    // CMSG_SUI_PARTY_TAXI flags
    enum Flags : uint8
    {
        FLAG_CONFIRMED = 0x01,          // fly the eligible members even if some cannot board
    };

    // SMSG_SUI_PARTY_TAXI_RESULT result
    enum Result : uint8
    {
        RESULT_FLYING         = 0,      // flights started; rows = members left behind
        RESULT_CONFIRM_NEEDED = 1,      // nobody flew; rows = members that cannot board
        RESULT_DENIED         = 2,      // bad request / flight master not reachable
        RESULT_NO_PATH        = 3,      // the node chain is not a taxi route
    };

    // per-member reason
    enum Reason : uint8
    {
        REASON_UNKNOWN_NODE = 1,        // has not discovered a node of the route
        REASON_NO_MONEY     = 2,
        REASON_TOO_FAR      = 3,        // not within boarding range of the flight master's node
        REASON_BUSY         = 4,        // dead, in combat, casting, teleporting
        REASON_IN_FLIGHT    = 5,        // already on a taxi
        REASON_OTHER_MAP    = 6,
        REASON_REFUSED      = 7,        // ActivateTaxiPathTo refused after the checks passed
    };

    static constexpr size_t MAX_NODES = 8;

    /// CMSG_SUI_PARTY_TAXI: u8 flags, u64 flightMaster, u8 nodeCount, u32 × nodes
    /// (source node first, destination last — the CMSG_ACTIVATETAXIEXPRESS chain).
    void HandlePartyTaxi(WorldSession* session, uint8 flags, ObjectGuid flightMaster,
        std::vector<uint32> const& nodes);
}

#endif
