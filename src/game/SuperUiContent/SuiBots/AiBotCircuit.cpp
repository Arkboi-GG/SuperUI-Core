/* ============================================================================
 * AiBotCircuit.cpp — registry, per-bot buffers, and the 1 Hz batch flush.
 * See AiBotCircuit.h for the contract; CIRCUIT_BOARD.md for the design.
 * ========================================================================== */

#include "AiBotCircuit.h"
#include "AiBotAIMain.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace CbCircuit
{
    volatile int g_mode = 0;

    // Explicit arms remain live when fleet shadow is off. The high-water mark
    // keeps the hot unarmed path proportional to the number of slots ever used
    // (four in the normal diagnostic setup), without taking a mutex per probe.
    static size_t const SHIP_GUID_CAP = 1024;
    static std::atomic<uint32_t> g_shipGuids[SHIP_GUID_CAP];
    static std::atomic<size_t> g_shipHighWater(0);
    static std::mutex g_shipMu;

    struct Site
    {
        const char* file;   // basename of __FILE__ (string literal — lives forever)
        int line;
        const char* desc;   // probe description (string literal)
    };

    struct HitRec
    {
        // Correlation ids and entity ids are exact below 2^53. Keeping this as
        // float collapsed thousands of distinct command ids to one value.
        double val;
        int site;
        uint8_t kind;       // 0 = bare, 1 = value, 2 = note
        char note[24];
    };

    struct BotBuf
    {
        std::mutex mu;
        std::vector<HitRec> hits;
        bool ship = false;
        int manifestMark = 0;   // sites [0..mark) already shipped on this connection
        uint32_t drops = 0;
    };

    static std::mutex g_siteMu;
    static std::vector<Site> g_sites;

    static std::mutex g_bufMu;
    static std::unordered_map<uint32_t, BotBuf*> g_bufs;

    static std::once_flag g_epochOnce;
    static char g_epoch[64];

    static const size_t HIT_CAP = 4096;      // per bot, per flush window (1s) — far above real volume
    static const size_t BATCH_HITS_MAX = 2048; // serialization cap per flush

    const char* Epoch()
    {
        std::call_once(g_epochOnce, []
        {
            // Wall-clock precision prevents ordinary restart collisions; the
            // process-local address adds an independent ASLR salt. The result is
            // an opaque JSON-safe token, not a timestamp contract.
            uint64_t const wall = static_cast<uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count());
            uintptr_t const salt = reinterpret_cast<uintptr_t>(&g_epoch);
            snprintf(g_epoch, sizeof(g_epoch), "%llx-%llx",
                static_cast<unsigned long long>(wall),
                static_cast<unsigned long long>(salt));
        });
        return g_epoch;
    }

    static const char* Basename(const char* path)
    {
        const char* s1 = strrchr(path, '/');
        const char* s2 = strrchr(path, '\\');
        const char* s = s1 > s2 ? s1 : s2;
        return s ? s + 1 : path;
    }

    static BotBuf* Buf(uint32_t guid)
    {
        std::lock_guard<std::mutex> lk(g_bufMu);
        BotBuf*& b = g_bufs[guid];
        if (!b) b = new BotBuf();
        return b;
    }

    int RegisterSite(const char* file, int line, const char* desc)
    {
        std::lock_guard<std::mutex> lk(g_siteMu);
        Site s;
        s.file = Basename(file);
        s.line = line;
        s.desc = desc;
        g_sites.push_back(s);
        return (int)g_sites.size();   // ids are 1-based inside Epoch()
    }

    static void Push(uint32_t guid, const HitRec& h)
    {
        BotBuf* b = Buf(guid);
        std::lock_guard<std::mutex> lk(b->mu);
        if (b->hits.size() >= HIT_CAP) { b->drops++; return; }
        b->hits.push_back(h);
    }

    void Hit(uint32_t guid, int siteId)
    {
        HitRec h; h.site = siteId; h.val = 0; h.kind = 0; h.note[0] = 0;
        Push(guid, h);
    }

    void HitV(uint32_t guid, int siteId, double value)
    {
        HitRec h; h.site = siteId; h.val = value; h.kind = 1; h.note[0] = 0;
        Push(guid, h);
    }

    void HitN(uint32_t guid, int siteId, const char* note)
    {
        HitRec h; h.site = siteId; h.val = 0; h.kind = 2;
        size_t i = 0;
        if (note)
            for (; i < sizeof(h.note) - 1 && note[i]; ++i)
            {
                char c = note[i];
                // JSON-safe subset; anything exotic becomes '_'
                h.note[i] = (c == '"' || c == '\\' || (unsigned char)c < 0x20) ? '_' : c;
            }
        h.note[i] = 0;
        Push(guid, h);
    }

    bool ShouldRecord(uint32_t guid)
    {
        if (g_mode)
            return true;
        size_t const n = g_shipHighWater.load(std::memory_order_acquire);
        for (size_t i = 0; i < n; ++i)
            if (g_shipGuids[i].load(std::memory_order_relaxed) == guid)
                return true;
        return false;
    }

    void SetMode(int mode) { g_mode = mode ? 1 : 0; }

    void SetShip(uint32_t guid, bool ship)
    {
        BotBuf* b = Buf(guid);
        {
            std::lock_guard<std::mutex> lk(b->mu);
            b->ship = ship;
        }

        std::lock_guard<std::mutex> lk(g_shipMu);
        size_t n = g_shipHighWater.load(std::memory_order_relaxed);
        if (ship)
        {
            size_t empty = n;
            for (size_t i = 0; i < n; ++i)
            {
                uint32_t const current = g_shipGuids[i].load(std::memory_order_relaxed);
                if (current == guid)
                    return;
                if (current == 0 && empty == n)
                    empty = i;
            }
            if (empty < n)
            {
                g_shipGuids[empty].store(guid, std::memory_order_release);
                return;
            }
            if (n < SHIP_GUID_CAP)
            {
                g_shipGuids[n].store(guid, std::memory_order_release);
                g_shipHighWater.store(n + 1, std::memory_order_release);
            }
            return;
        }

        for (size_t i = 0; i < n; ++i)
            if (g_shipGuids[i].load(std::memory_order_relaxed) == guid)
                g_shipGuids[i].store(0, std::memory_order_release);
        while (n > 0 && g_shipGuids[n - 1].load(std::memory_order_relaxed) == 0)
            --n;
        g_shipHighWater.store(n, std::memory_order_release);
    }

    void ResetManifest(uint32_t guid)
    {
        BotBuf* b = Buf(guid);
        std::lock_guard<std::mutex> lk(b->mu);
        b->manifestMark = 0;
    }

    void Flush(uint32_t guid, int mapId, int zoneId, float x, float y, float z,
               std::vector<std::string>& out)
    {
        if (!ShouldRecord(guid))
            return;

        BotBuf* b = Buf(guid);
        std::vector<HitRec> hits;
        bool ship;
        uint32_t drops;
        int mark;
        {
            std::lock_guard<std::mutex> lk(b->mu);
            ship = b->ship;
            drops = b->drops;
            b->drops = 0;
            mark = b->manifestMark;
            hits.swap(b->hits);   // always drain — an unshipped bot stays O(1) memory
        }
        if (!ship || hits.empty())
            return;

        char const* epoch = Epoch();
        char line[512];

        // 1. Any site defs this connection hasn't shipped yet (self-decoding stream).
        {
            std::lock_guard<std::mutex> lk(g_siteMu);
            int total = (int)g_sites.size();
            for (int i = mark; i < total; ++i)
            {
                Site const& s = g_sites[(size_t)i];
                snprintf(line, sizeof(line),
                    "{\"type\":\"CIRCUIT_SITE\",\"payload\":{\"circuitEpoch\":\"%s\",\"guid\":%u,\"id\":%d,\"file\":\"%s\",\"line\":%d,\"desc\":\"%s\"}}",
                    epoch, guid, i + 1, s.file, s.line, s.desc);
                out.push_back(line);
            }
            std::lock_guard<std::mutex> lk2(b->mu);
            b->manifestMark = total;
        }

        // 2. The batch: one envelope carrying this second's hits + position (R10).
        size_t const shown = hits.size() < BATCH_HITS_MAX ? hits.size() : BATCH_HITS_MAX;
        // The serialization tail used to disappear without contributing to drops.
        drops += static_cast<uint32_t>(hits.size() - shown);
        std::string batch;
        batch.reserve(96 + shown * 20);
        snprintf(line, sizeof(line),
            "{\"type\":\"CIRCUIT_BATCH\",\"payload\":{\"circuitEpoch\":\"%s\",\"guid\":%u,\"map\":%d,\"zone\":%d,\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"drops\":%u,\"h\":[",
            epoch, guid, mapId, zoneId, x, y, z, drops);
        batch += line;

        for (size_t i = 0; i < shown; ++i)
        {
            HitRec const& h = hits[i];
            if (i) batch += ',';
            if (h.kind == 1)
            {
                if (!std::isfinite(h.val))
                    snprintf(line, sizeof(line), "[%d,null]", h.site);
                else if (h.val == std::floor(h.val) && std::fabs(h.val) < 9007199254740992.0)
                    snprintf(line, sizeof(line), "[%d,%.0f]", h.site, h.val);
                else
                    snprintf(line, sizeof(line), "[%d,%.17g]", h.site, h.val);
            }
            else if (h.kind == 2)
                snprintf(line, sizeof(line), "[%d,null,\"%s\"]", h.site, h.note);
            else
                snprintf(line, sizeof(line), "[%d]", h.site);
            batch += line;
        }
        batch += "]}}";
        out.push_back(std::move(batch));
    }
}

/* ── AiBotAI glue ─────────────────────────────────────────────────────────── */

/* Ship buffered probes once per second from UpdateBridgeTick. Fleet shadow may
 * be off; an explicitly armed bot still records and ships. */
void AiBotAI::CircuitFlush()
{
    if (!m_bridgeConnected || !me || !CbCircuit::ShouldRecord(me->GetGUIDLow()))
        return;
    std::vector<std::string> lines;
    CbCircuit::Flush(me->GetGUIDLow(), (int)me->GetMapId(), (int)me->GetZoneId(),
                     me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), lines);
    for (size_t i = 0; i < lines.size(); ++i)
        BridgeSend(lines[i].c_str());
}
