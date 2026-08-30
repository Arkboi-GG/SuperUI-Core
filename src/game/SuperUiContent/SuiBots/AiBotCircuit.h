#pragma once
/* ============================================================================
 * AiBotCircuit — the C++ probe side of the circuit board (CIRCUIT_BOARD.md).
 *
 * Mirror of the C# CircuitTrace facade. A probe is one macro dropped into a
 * branch arm:
 *
 *     CB_HIT(me->GetGUIDLow(),  "cpp-move: rejected, in combat");
 *     CB_HITV(me->GetGUIDLow(), "cpp-grind: target acquired", entry);
 *     CB_HITN(me->GetGUIDLow(), "cpp-bridge: dispatch", msgType);
 *
 * Identity is the CALL SITE (__FILE__/__LINE__, design rule R4): the site
 * registers itself on its first recording pass and gets a process-epoch-scoped id.
 * Mode 0 disables fleet-wide shadow recording but explicitly armed bots still
 * record; mode 1 buffers hits for every bot. Bots the C# side ARMED (ship=1) have
 * their buffer serialized once per second on the bridge tick and sent as
 * CIRCUIT_SITE / CIRCUIT_BATCH lines, which C# merges into the bot's single
 * timeline (rule R1) with this side's ids offset into their own space (R3).
 *
 * Chain stitching (R2): C# stamps "cbt" on every outbound envelope;
 * BridgeProcessLine records it via a "cpp-chain: command adopted" probe whose
 * VALUE is the chain id — the C# viewer matches it to its own
 * "chain: command sent" probe carrying the same value.
 *
 * Control: the CIRCUIT_TRACE command ({"mode":0|1,"ship":0|1}) — mode is
 * global, ship is per-bot. C# pushes both on HELLO and on every toggle, so
 * one switch arms the whole nervous system (rule R6).
 * ========================================================================== */

#include <cstdint>
#include <string>
#include <vector>

namespace CbCircuit
{
    extern volatile int g_mode;   // 0 = armed-only, 1 = fleet shadow (ship only armed bots)

    // Opaque identity for this mangosd process. Numeric probe ids are only
    // meaningful inside this epoch; HELLO, SITE, and BATCH all carry it so a
    // C++ restart can never reinterpret an old trace label in a surviving C# host.
    const char* Epoch();

    int  RegisterSite(const char* file, int line, const char* desc);
    void Hit(uint32_t guid, int siteId);
    void HitV(uint32_t guid, int siteId, double value);
    void HitN(uint32_t guid, int siteId, const char* note);

    bool ShouldRecord(uint32_t guid);
    void SetMode(int mode);
    void SetShip(uint32_t guid, bool ship);
    void ResetManifest(uint32_t guid);   // after a (re)connect: re-ship site defs to C#

    // Drain this bot's buffered hits (plus any site defs it hasn't shipped on
    // this connection) into complete JSON envelope lines, oldest first. The
    // caller (AiBotAI::CircuitFlush, once per second on the bridge tick) sends
    // each through its own BridgeSend.
    void Flush(uint32_t guid, int mapId, int zoneId, float x, float y, float z,
               std::vector<std::string>& out);
}

/* Probe macros — the site id caches in a function-local static on the first
 * recording pass (thread-safe magic static). With fleet shadow off, an unarmed
 * probe performs only the small lock-free armed-guid scan. */
#define CB_HIT(guid, desc) \
    do { uint32_t const _cbGuid = (guid); if (CbCircuit::ShouldRecord(_cbGuid)) { static const int _cbSite = CbCircuit::RegisterSite(__FILE__, __LINE__, (desc)); CbCircuit::Hit(_cbGuid, _cbSite); } } while (0)
#define CB_HITV(guid, desc, val) \
    do { uint32_t const _cbGuid = (guid); if (CbCircuit::ShouldRecord(_cbGuid)) { static const int _cbSite = CbCircuit::RegisterSite(__FILE__, __LINE__, (desc)); CbCircuit::HitV(_cbGuid, _cbSite, (double)(val)); } } while (0)
#define CB_HITN(guid, desc, note) \
    do { uint32_t const _cbGuid = (guid); if (CbCircuit::ShouldRecord(_cbGuid)) { static const int _cbSite = CbCircuit::RegisterSite(__FILE__, __LINE__, (desc)); CbCircuit::HitN(_cbGuid, _cbSite, (note)); } } while (0)
