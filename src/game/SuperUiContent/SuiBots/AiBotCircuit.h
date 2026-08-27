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
 * registers itself on its first armed pass and gets a session-scoped id.
 * When g_mode is 0 (off) a probe costs exactly one volatile load + branch.
 * Mode 1 (shadow) buffers hits per bot; bots the C# side ARMED (ship=1) have
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
    extern volatile int g_mode;   // 0 = off, 1 = shadow (record; ship only armed bots)

    int  RegisterSite(const char* file, int line, const char* desc);
    void Hit(uint32_t guid, int siteId);
    void HitV(uint32_t guid, int siteId, double value);
    void HitN(uint32_t guid, int siteId, const char* note);

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
 * ARMED pass (thread-safe magic static); a disarmed probe never registers. */
#define CB_HIT(guid, desc) \
    do { if (CbCircuit::g_mode) { static const int _cbSite = CbCircuit::RegisterSite(__FILE__, __LINE__, (desc)); CbCircuit::Hit((guid), _cbSite); } } while (0)
#define CB_HITV(guid, desc, val) \
    do { if (CbCircuit::g_mode) { static const int _cbSite = CbCircuit::RegisterSite(__FILE__, __LINE__, (desc)); CbCircuit::HitV((guid), _cbSite, (double)(val)); } } while (0)
#define CB_HITN(guid, desc, note) \
    do { if (CbCircuit::g_mode) { static const int _cbSite = CbCircuit::RegisterSite(__FILE__, __LINE__, (desc)); CbCircuit::HitN((guid), _cbSite, (note)); } } while (0)
