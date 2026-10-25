/*
 * WoW Karting - the race (MSUIClient shared_docs/WOW_KARTING.md, Phase 4).
 *
 * One server-authoritative race at a time on the course the `wow-karting` World
 * Content Pack defines as game_tele rows (kart:<course>:meta/grid/cp/line/portal).
 * Without the pack there is no course and nothing here runs.
 *
 *   Gathering  racers join (the marshal's gossip, /kart join); bots fill on a start request
 *   Countdown  everyone on the grid, rooted; Daisy calls 3, 2, 1, GO one second apart
 *   Racing     checkpoints in order, laps on the start/finish line, ranks; a racer knocked off
 *              the kart is put back on after a penalty, a dead one is out (no god mode)
 *   Finished   results for everyone, then the bots log out
 *
 * Threads: SuiKarting::Tick runs on the world thread after the map update (players, packets,
 * teleports); SuiKarting::TickRacer runs inside a racer bot's AiBotAI::UpdateAI on its map's
 * thread and only drives that bot. Both take g_lock.
 */

#include "SuiKarting.h"

#include "AiBotAIMain.h"
#include "Creature.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerBotMgr.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "SpellMgr.h"
#include "SuperUiContent/SuiWorld/Bridge/SuiPortal.h"
#include "Timer.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    enum class Phase : uint8 { Idle = 0, Gathering = 1, Countdown = 2, Racing = 3, Finished = 4 };
    enum Action : uint8 { ACTION_JOIN = 1, ACTION_LEAVE = 2, ACTION_START = 3, ACTION_QUERY = 4, ACTION_USE = 5 };
    enum Kind : uint8 { KIND_STATE = 1, KIND_COUNTDOWN = 2, KIND_PREWARM = 3, KIND_NOTICE = 4, KIND_ITEM = 5 };
    enum Notice : uint8
    {
        NOTICE_JOINED = 1, NOTICE_LEFT = 2, NOTICE_NOT_ON_KART = 3, NOTICE_FULL = 4, NOTICE_NO_COURSE = 5,
        NOTICE_LAP = 6, NOTICE_FINISHED = 7, NOTICE_DNF = 8,
    };
    enum RacerFlag : uint8 { RF_BOT = 1, RF_FINISHED = 2, RF_DNF = 4, RF_SELF = 8 };

    constexpr uint8 WIRE_VERSION = 1;
    constexpr uint32 KART_GOBLIN = 38001;
    constexpr uint32 KART_GNOME = 38002;
    constexpr uint32 KART_RACING = 38003;   // the custom go-kart (pack display 71000, kart-model.py)
    constexpr uint32 DAISY_ENTRY = 4507;          // Mirage Raceway "Race Starter Girl"
    constexpr uint32 MARSHAL_TEXT = 7100000;      // pack npc_text
    constexpr uint32 GATHER_MS = 30000;           // a lone joiner waits this long for company
    constexpr uint32 FILL_GATHER_MS = 8000;       // after a start request: time for bots to log in
    constexpr uint32 GRID_LEAD_MS = 3000;         // on the grid before "3"
    constexpr uint32 BEAT_MS = 1000;
    constexpr uint32 FINISH_GRACE_MS = 90000;     // after the first finisher, the rest have this long
    constexpr uint32 RESULTS_MS = 30000;
    constexpr uint32 PUSH_MS = 500;
    constexpr uint32 REMOUNT_MS = 3000;           // knocked off the kart: back on after this
    constexpr uint32 RACE_TIMEOUT_MS = 20 * 60 * 1000;
    constexpr uint8 MAX_RACERS = 8;
    constexpr uint8 FILL_TO = 6;
    constexpr uint32 BOT_POINT_ID = 0x4B415254;   // 'KART'
    constexpr float ARRIVAL_YARDS = 45.0f;        // a racer is on leg L once within this of L's first line point

    struct Point { uint32 map = 0; float x = 0, y = 0, z = 0, o = 0; };

    // ── Phase 5: the Mario Kart 64 items (MSUIClient shared_docs/WOW_KARTING.md §3b) ─────────────
    //
    // Mechanics are MK64's; everything seen is WoW: the item box is the battleground speed rune, the
    // banana a real banana, the shells green/red/blue turtles, the boost the Mirage Raceway's own
    // Racer Cheer, the spin-out its Damage Car, lightning a Shrink Ray. Boxes, peels and fake boxes
    // are summoned gameobjects; shells are summoned creatures, so every client sees them for free.

    constexpr uint32 BOX_RESPAWN_MS = 2000;
    constexpr uint32 ROULETTE_MS = 2000;
    constexpr uint32 HIT_IMMUNE_MS = 1200;        // after a hit, the kart is not hit again at once
    constexpr uint32 GOLDEN_MS = 7500;
    constexpr uint32 HAZARD_LIFE_MS = 90000;
    constexpr float BOX_RADIUS = 3.5f;
    constexpr float HAZARD_RADIUS = 2.5f;
    constexpr float SHELL_RADIUS = 3.0f;
    constexpr float STAR_RADIUS = 3.0f;
    constexpr float SPINY_BLAST_RADIUS = 10.0f;
    constexpr float GREEN_SPEED = 32.0f;
    constexpr float RED_SPEED = 30.0f;
    constexpr float SPINY_SPEED = 42.0f;

    constexpr uint32 SPELL_BOOST = 38201;   // Racer Cheer: +119 % speed, flames, 1.5 s
    constexpr uint32 SPELL_SPIN = 38202;    // banana: stunned 1 s, Damage Car smoke
    constexpr uint32 SPELL_TUMBLE = 38203;  // shells, fake box, squash: stunned 1.5 s
    constexpr uint32 SPELL_STAR = 38204;    // star: +35 % speed, glow, 7 s
    constexpr uint32 SPELL_SHRUNK = 38205;  // lightning: scale -51 %, slow, 5 s
    constexpr uint32 SPELL_BOO = 38206;     // Boo: invisible, 5 s

    constexpr uint32 GO_ITEM_BOX = 7100110;
    constexpr uint32 GO_BANANA = 7100111;
    constexpr uint32 GO_FAKE_BOX = 7100112;
    constexpr uint32 NPC_GREEN_SHELL = 7100010;
    constexpr uint32 NPC_RED_SHELL = 7100011;
    constexpr uint32 NPC_SPINY_SHELL = 7100012;

    enum Item : uint8
    {
        ITEM_NONE = 0, ITEM_BANANA, ITEM_BANANA_BUNCH, ITEM_GREEN, ITEM_TRIPLE_GREEN, ITEM_RED, ITEM_TRIPLE_RED,
        ITEM_SPINY, ITEM_MUSHROOM, ITEM_TRIPLE_MUSHROOM, ITEM_GOLDEN, ITEM_STAR, ITEM_LIGHTNING, ITEM_FAKE_BOX, ITEM_BOO,
        ITEM_COUNT
    };

    uint8 UsesOf(uint8 item)
    {
        switch (item)
        {
            case ITEM_BANANA_BUNCH: return 5;
            case ITEM_TRIPLE_GREEN: case ITEM_TRIPLE_RED: case ITEM_TRIPLE_MUSHROOM: return 3;
            case ITEM_NONE: return 0;
            default: return 1;
        }
    }

    struct Checkpoint { Point p; float radius = 45.0f; uint8 leg = 0; };

    struct Course
    {
        bool loaded = false;
        bool attempted = false;
        std::string key;
        uint8 laps = 3;
        Point line;
        std::vector<Point> grid;
        std::vector<Checkpoint> cps;
        std::vector<std::vector<Point>> lines;    // per leg; the last point is the leg's portal
        std::vector<uint32> portalEntry;          // per leg
        std::vector<Point> boxes;                 // item box spots
    };

    struct Racer
    {
        ObjectGuid guid;
        bool bot = false;
        std::string name;
        uint32 kart = KART_RACING;
        uint8 lap = 1;
        uint8 nextCp = 1;
        uint8 leg = 0;
        uint16 linePt = 0;
        uint32 finishMs = 0;
        bool dnf = false;
        uint8 rank = 0;
        uint32 offKartSince = 0;
        uint32 lastPush = 0;
        uint8 prewarmedLeg = 0xFF;
        float skill = 1.0f;
        uint32 lastOrderAt = 0;
        uint16 orderedPt = 0xFFFF;
        uint8 orderedLeg = 0xFF;
        bool held = false;
        float orderedSpeed = 0.0f;
        // items
        uint8 item = 0;
        uint8 uses = 0;
        uint32 rouletteUntil = 0;
        uint32 goldenUntil = 0;
        uint32 lastHitAt = 0;
        uint32 itemSince = 0;
        uint32 lastUseAt = 0;
        uint8 sentItem = 0xFF;
        uint8 sentUses = 0xFF;
        bool sentRoulette = false;
    };

    struct BoxSlot { Point p; ObjectGuid go; uint32 hiddenUntil = 0; };
    struct Hazard { uint8 type = 0; ObjectGuid go; ObjectGuid owner; uint32 map = 0; float x = 0, y = 0; uint32 bornAt = 0; };
    struct Shell
    {
        uint8 type = 0; ObjectGuid creature; ObjectGuid owner; ObjectGuid target; uint32 map = 0; uint8 leg = 0;
        uint16 linePt = 0; uint16 orderedPt = 0xFFFF; uint32 orderedAt = 0; uint32 bornAt = 0; uint32 lifeMs = 0;
        bool backward = false;
    };
    struct Blast { ObjectGuid target; uint32 at = 0; };

    struct Race
    {
        uint32 id = 0;
        Phase phase = Phase::Idle;
        uint32 phaseAt = 0;
        uint32 goAt = 0;
        int8 beat = -1;
        bool startRequested = false;
        uint32 firstFinishAt = 0;
        uint8 finishedCount = 0;
        std::vector<Racer> racers;
        std::vector<std::string> pendingBots;   // logging in
        std::vector<BoxSlot> boxes;
        std::vector<Hazard> hazards;
        std::vector<Shell> shells;
        std::vector<Blast> blasts;
    };

    struct BotSeed { char const* name; uint8 race; uint8 cls; uint32 kart; float skill; };
    // Racer bots: created once (a SuperUI AiBotAI each), logged in for a race and out after it.
    constexpr BotSeed BOT_SEEDS[] = {
        { "Gearspark", 7, 1, KART_RACING, 0.97f },  { "Boltzap", 7, 4, KART_RACING, 0.95f },
        { "Fizzwick", 7, 8, KART_RACING, 0.93f },   { "Cograttle", 7, 1, KART_RACING, 0.96f },
        { "Sprocka", 7, 9, KART_RACING, 0.94f },    { "Turbonk", 7, 4, KART_RACING, 0.98f },
        { "Nitrocog", 7, 8, KART_RACING, 0.92f },
    };

    std::recursive_mutex g_lock;
    Course g_course;
    Race g_race;
    uint32 g_nextRaceId = 1;

    uint32 Now() { return WorldTimer::getMSTime(); }

    float Dist2d(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

    // ── course ─────────────────────────────────────────────────────────────

    std::vector<std::string> Split(std::string const& s, char sep)
    {
        std::vector<std::string> out;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i)
            if (i == s.size() || s[i] == sep) { out.push_back(s.substr(start, i - start)); start = i + 1; }
        return out;
    }

    void LoadCourse()
    {
        g_course.attempted = true;
        Course c;
        std::map<uint32, Point> grid;
        std::map<uint32, Checkpoint> cps;
        std::map<uint32, std::map<uint32, Point>> lines;
        std::map<uint32, uint32> portals;
        for (auto const& kv : sObjectMgr.GetGameTeleMap())
        {
            GameTele const& t = kv.second;
            if (t.name.rfind("kart:", 0) != 0)
                continue;
            std::vector<std::string> f = Split(t.name, ':');
            if (f.size() < 4)
                continue;
            if (c.key.empty())
                c.key = f[1];
            if (f[1] != c.key)
                continue;
            Point p{ t.mapId, t.x, t.y, t.z, t.o };
            try
            {
                if (f[2] == "meta" && f[3].rfind("laps=", 0) == 0)
                {
                    c.laps = uint8(std::max(1, std::min(9, std::stoi(f[3].substr(5)))));
                    c.line = p;
                }
                else if (f[2] == "grid")
                    grid[std::stoul(f[3])] = p;
                else if (f[2] == "cp")
                {
                    Checkpoint cp;
                    cp.p = p;
                    if (f.size() > 4 && f[4].size() > 1 && f[4][0] == 'r')
                        cp.radius = std::stof(f[4].substr(1));
                    cps[std::stoul(f[3])] = cp;
                }
                else if (f[2] == "line" && f.size() > 4)
                    lines[std::stoul(f[3])][std::stoul(f[4])] = p;
                else if (f[2] == "box")
                    c.boxes.push_back(p);
                else if (f[2] == "portal" && f.size() > 4)
                    portals[std::stoul(f[3])] = std::stoul(f[4]);
            }
            catch (...) { /* a malformed row is skipped */ }
        }
        for (auto const& kv : grid) c.grid.push_back(kv.second);
        for (auto const& kv : cps) c.cps.push_back(kv.second);
        for (auto const& kv : lines)
        {
            std::vector<Point> leg;
            for (auto const& pt : kv.second) leg.push_back(pt.second);
            c.lines.push_back(leg);
            c.portalEntry.push_back(portals.count(kv.first) ? portals[kv.first] : 0);
        }
        // Which leg each checkpoint belongs to: the leg whose racing line passes nearest it.
        for (Checkpoint& cp : c.cps)
        {
            float best = 1e9f;
            for (size_t l = 0; l < c.lines.size(); ++l)
                for (Point const& pt : c.lines[l])
                    if (pt.map == cp.p.map)
                    {
                        float d = Dist2d(pt.x, pt.y, cp.p.x, cp.p.y);
                        if (d < best) { best = d; cp.leg = uint8(l); }
                    }
        }
        c.loaded = !c.grid.empty() && c.cps.size() >= 2 && !c.lines.empty();
        if (c.loaded)
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[KARTING] course '%s': %u laps, %u grid slot(s), %u checkpoint(s), %u leg(s)",
                c.key.c_str(), c.laps, uint32(c.grid.size()), uint32(c.cps.size()), uint32(c.lines.size()));
        c.attempted = true;
        g_course = c;
    }

    // ── packets ───────────────────────────────────────────────────────────

    bool Capable(Player* p) { return p && p->GetSession() && !p->GetSession()->GetBot() && p->GetSession()->IsSuiCapable(); }

    void SendNotice(Player* p, uint8 code, uint32 param = 0)
    {
        if (!Capable(p)) return;
        WorldPacket data(SMSG_SUI_KART, 7);
        data << uint8(WIRE_VERSION) << uint8(KIND_NOTICE) << uint8(code) << uint32(param);
        p->GetSession()->SendPacket(&data);
    }

    void SendBeat(Player* p, uint8 count)
    {
        if (!Capable(p)) return;
        WorldPacket data(SMSG_SUI_KART, 7);
        data << uint8(WIRE_VERSION) << uint8(KIND_COUNTDOWN) << uint32(g_race.id) << uint8(count);
        p->GetSession()->SendPacket(&data);
    }

    void SendPrewarm(Player* p, uint8 leg)
    {
        if (!Capable(p) || leg >= g_course.portalEntry.size()) return;
        uint32 entry = g_course.portalEntry[leg];
        GameObjectInfo const* info = entry ? sObjectMgr.GetGameObjectTemplate(entry) : nullptr;
        if (!info || !SuiPortal::IsPublicPackPortal(entry, info)) return;
        SpellTargetPosition const* dest = sSpellMgr.GetSpellTargetPosition(info->spellcaster.spellId);
        if (!dest) return;
        WorldPacket data(SMSG_SUI_KART, 34);
        data << uint8(WIRE_VERSION) << uint8(KIND_PREWARM) << uint32(entry) << uint32(info->spellcaster.spellId)
             << uint32(dest->mapId) << float(dest->x) << float(dest->y) << float(dest->z) << float(dest->o);
        p->GetSession()->SendPacket(&data);
    }

    void SendState(Player* p, Racer const* self)
    {
        if (!Capable(p)) return;
        uint32 now = Now();
        int32 countdown = 0;
        if (g_race.phase == Phase::Countdown)
            countdown = int32(g_race.phaseAt + GRID_LEAD_MS + 3 * BEAT_MS) - int32(now);
        uint32 elapsed = g_race.goAt && g_race.phase >= Phase::Racing ? WorldTimer::getMSTimeDiff(g_race.goAt, now) : 0;
        uint8 count = uint8(std::min<size_t>(g_race.racers.size(), 16));
        WorldPacket data(SMSG_SUI_KART, 20 + count * 17 + 21);
        data << uint8(WIRE_VERSION) << uint8(KIND_STATE) << uint32(g_race.id) << uint8(g_race.phase)
             << uint8(g_course.laps) << uint8(g_course.cps.size()) << int32(countdown) << uint32(elapsed) << uint8(count);
        for (uint8 i = 0; i < count; ++i)
        {
            Racer const& r = g_race.racers[i];
            uint8 flags = (r.bot ? RF_BOT : 0) | (r.finishMs ? RF_FINISHED : 0) | (r.dnf ? RF_DNF : 0) |
                (self && r.guid == self->guid ? RF_SELF : 0);
            data << uint64(r.guid.GetRawValue()) << uint8(r.lap) << uint8(r.nextCp) << uint8(r.rank) << uint8(flags)
                 << uint32(r.finishMs);
        }
        if (self && !self->finishMs && !self->dnf && self->nextCp < g_course.cps.size())
        {
            Checkpoint const& cp = g_course.cps[self->nextCp];
            data << uint8(1) << uint32(cp.p.map) << float(cp.p.x) << float(cp.p.y) << float(cp.p.z) << float(cp.radius);
        }
        else
            data << uint8(0);
        p->GetSession()->SendPacket(&data);
    }

    void PushAll(bool force)
    {
        uint32 now = Now();
        for (Racer& r : g_race.racers)
        {
            if (r.bot || (!force && WorldTimer::getMSTimeDiff(r.lastPush, now) < PUSH_MS)) continue;
            if (Player* p = ObjectAccessor::FindPlayer(r.guid))
            {
                SendState(p, &r);
                r.lastPush = now;
            }
        }
    }

    // ── racers ────────────────────────────────────────────────────────────

    Racer* FindRacer(ObjectGuid guid)
    {
        for (Racer& r : g_race.racers)
            if (r.guid == guid) return &r;
        return nullptr;
    }

    bool OnKart(Player* p) { return SuiKarting::IsNeutralKartRider(p); }

    void Mount(Player* p, uint32 kart)
    {
        if (!p || OnKart(p)) return;
        if (p->IsMounted()) p->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
        p->CastSpell(p, kart, true);
    }

    void Yell(char const* text)
    {
        // Daisy, the Mirage Raceway's own starter, calls it in-world.
        for (Racer const& r : g_race.racers)
            if (Player* p = ObjectAccessor::FindPlayer(r.guid))
                if (p->IsInWorld() && p->GetMapId() == g_course.line.map)
                    if (Creature* daisy = p->FindNearestCreature(DAISY_ENTRY, 250.0f))
                    {
                        daisy->MonsterYell(text);
                        return;
                    }
    }

    void Teleport(Player* p, Point const& at)
    {
        if (p->GetMapId() == at.map)
            p->NearTeleportTo(at.x, at.y, at.z, at.o);
        else
            p->TeleportTo(at.map, at.x, at.y, at.z, at.o);
    }

    void SetHeld(Player* p, bool held, bool bot)
    {
        if (bot)
        {
            if (held) { p->StopMoving(); p->GetMotionMaster()->MoveIdle(); }
            return;   // a bot never acks a root; TickRacer holds it instead
        }
        p->SetRooted(held);
    }

    void SpawnBots(uint8 wanted)
    {
        for (BotSeed const& seed : BOT_SEEDS)
        {
            if (wanted == 0) break;
            std::string name = seed.name;
            bool already = false;
            for (Racer const& r : g_race.racers) already |= r.name == name;
            for (std::string const& n : g_race.pendingBots) already |= n == name;
            if (already) continue;
            Point at = g_course.grid.front();
            if (!ObjectAccessor::FindPlayerByName(name.c_str()))
            {
                uint32 instance = sMapMgr.GetContinentInstanceId(at.map, at.x, at.y);
                AiBotAI* ai = new AiBotAI(seed.race, seed.cls, 60, at.map, instance, at.x, at.y, at.z, at.o);
                ai->SetSpawnName(name);
                ObjectGuid existing = sObjectMgr.GetPlayerGuidByName(name);
                bool ok = existing ? sPlayerBotMgr.AddBot(existing.GetCounter(), false, ai) : sPlayerBotMgr.AddBot(ai);
                if (!ok)
                {
                    sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[KARTING] racer bot %s could not log in", name.c_str());
                    continue;
                }
            }
            g_race.pendingBots.push_back(name);
            --wanted;
        }
    }

    void AdoptPendingBots()
    {
        for (auto it = g_race.pendingBots.begin(); it != g_race.pendingBots.end();)
        {
            Player* p = ObjectAccessor::FindPlayerByName(it->c_str());
            if (!p || !p->IsInWorld() || p->IsBeingTeleported() || g_race.racers.size() >= MAX_RACERS)
            {
                ++it;
                continue;
            }
            Racer r;
            r.guid = p->GetObjectGuid();
            r.bot = true;
            r.name = *it;
            for (BotSeed const& seed : BOT_SEEDS)
                if (r.name == seed.name) { r.kart = seed.kart; r.skill = seed.skill; }
            if (AiBotAI* ai = dynamic_cast<AiBotAI*>(p->AI()))
                ai->SuiAbandonJourney();
            g_race.racers.push_back(r);
            it = g_race.pendingBots.erase(it);
        }
    }

    void LogOutBots()
    {
        for (Racer const& r : g_race.racers)
            if (r.bot)
                sPlayerBotMgr.DeleteBot(r.guid.GetCounter());
    }

    void ClearItems();

    void ResetRace()
    {
        ClearItems();
        for (Racer const& r : g_race.racers)
            if (!r.bot)
                if (Player* p = ObjectAccessor::FindPlayer(r.guid))
                    p->SetRooted(false);
        LogOutBots();
        g_race = Race();
    }

    // ── the race ──────────────────────────────────────────────────────────

    void BeginCountdown()
    {
        AdoptPendingBots();
        g_race.pendingBots.clear();
        g_race.phase = Phase::Countdown;
        g_race.phaseAt = Now();
        g_race.beat = -1;
        for (Point const& b : g_course.boxes)
            g_race.boxes.push_back({ b, ObjectGuid(), 0 });
        // The grid: humans first (pole), then bots.
        std::stable_sort(g_race.racers.begin(), g_race.racers.end(), [](Racer const& a, Racer const& b) { return !a.bot && b.bot; });
        for (size_t i = 0; i < g_race.racers.size(); ++i)
        {
            Racer& r = g_race.racers[i];
            Player* p = ObjectAccessor::FindPlayer(r.guid);
            if (!p) continue;
            Point slot = g_course.grid[i % g_course.grid.size()];
            Teleport(p, slot);
            Mount(p, r.kart);
            SetHeld(p, true, r.bot);
            r.lap = 1;
            r.nextCp = 1;
            r.leg = 0;
            // The bot aims at the first racing-line point ahead of its slot: the nearest one, or the next
            // when that nearest one is under its wheels.
            std::vector<Point> const& line = g_course.lines[0];
            uint16 best = 0;
            float bestD = 1e9f;
            for (uint16 k = 0; k < line.size(); ++k)
            {
                float d = Dist2d(line[k].x, line[k].y, slot.x, slot.y);
                if (d < bestD) { bestD = d; best = k; }
            }
            r.linePt = uint16(std::min<size_t>(line.size() - 1, best + (bestD < 8.0f ? 1 : 0)));
        }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[KARTING] race %u: %u racer(s) on the grid", g_race.id, uint32(g_race.racers.size()));
        PushAll(true);
    }

    void Go()
    {
        g_race.phase = Phase::Racing;
        g_race.goAt = Now();
        for (Racer& r : g_race.racers)
            if (Player* p = ObjectAccessor::FindPlayer(r.guid))
            {
                SetHeld(p, false, r.bot);
                SendPrewarm(p, 0);
                r.prewarmedLeg = 0;
            }
        PushAll(true);
    }

    uint32 Progress(Racer const& r)
    {
        // laps done, then checkpoints passed this lap (nextCp 0 = all passed, heading for the line)
        uint32 cps = uint32(g_course.cps.size());
        uint32 passed = r.nextCp == 0 ? cps : r.nextCp;
        return uint32(r.lap) * 1000u + passed;
    }

    void Rank()
    {
        std::vector<Racer*> order;
        for (Racer& r : g_race.racers) order.push_back(&r);
        auto distanceToNext = [](Racer const* r) -> float
        {
            Player* p = ObjectAccessor::FindPlayer(r->guid);
            if (!p || r->nextCp >= g_course.cps.size()) return 1e9f;
            Checkpoint const& cp = g_course.cps[r->nextCp];
            return p->GetMapId() == cp.p.map ? Dist2d(p->GetPositionX(), p->GetPositionY(), cp.p.x, cp.p.y) : 1e8f;
        };
        std::stable_sort(order.begin(), order.end(), [&](Racer const* a, Racer const* b)
        {
            if (a->dnf != b->dnf) return !a->dnf;
            if ((a->finishMs != 0) != (b->finishMs != 0)) return a->finishMs != 0;
            if (a->finishMs && b->finishMs) return a->finishMs < b->finishMs;
            if (Progress(*a) != Progress(*b)) return Progress(*a) > Progress(*b);
            return distanceToNext(a) < distanceToNext(b);
        });
        for (size_t i = 0; i < order.size(); ++i) order[i]->rank = uint8(i + 1);
    }

    void Finish(Racer& r, Player* p)
    {
        r.finishMs = WorldTimer::getMSTimeDiff(g_race.goAt, Now());
        ++g_race.finishedCount;
        if (!g_race.firstFinishAt) g_race.firstFinishAt = Now();
        Rank();
        if (p) SendNotice(p, NOTICE_FINISHED, r.rank);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[KARTING] race %u: %s finished %u in %u ms", g_race.id, r.name.c_str(), r.rank, r.finishMs);
    }

    void UpdateRacer(Racer& r)
    {
        if (r.finishMs || r.dnf) return;
        // A racer crossing to another continent is OUT of the world for a moment (FindPlayer misses it):
        // only a logout is a DNF (2026-10-04 live: race 1 dropped its only human at the first portal).
        Player* p = ObjectAccessor::FindPlayerNotInWorld(r.guid);
        if (!p)
        {
            r.dnf = true;
            return;
        }
        if (!p->IsInWorld()) return;
        if (!p->IsAlive())
        {
            r.dnf = true;
            SendNotice(p, NOTICE_DNF);
            return;
        }
        if (p->IsBeingTeleported()) return;
        uint32 now = Now();
        // Knocked off the kart (Dazed, a GM, a player): back on after a penalty, like a kart fished out of a lake.
        if (!OnKart(p))
        {
            if (!r.offKartSince) r.offKartSince = now;
            else if (WorldTimer::getMSTimeDiff(r.offKartSince, now) >= REMOUNT_MS)
            {
                Mount(p, r.kart);
                r.offKartSince = 0;
            }
        }
        else
            r.offKartSince = 0;

        // Which leg: the leg whose arrival point the racer stands near (a portal just put it there).
        for (uint8 l = 0; l < g_course.lines.size(); ++l)
        {
            Point const& arrive = g_course.lines[l].front();
            if (l != r.leg && p->GetMapId() == arrive.map &&
                Dist2d(p->GetPositionX(), p->GetPositionY(), arrive.x, arrive.y) < ARRIVAL_YARDS &&
                (l == (r.leg + 1) % g_course.lines.size()))
            {
                r.leg = l;
                if (r.bot) r.linePt = 1;
            }
        }
        if (!r.bot && r.prewarmedLeg != r.leg)
        {
            SendPrewarm(p, r.leg);
            r.prewarmedLeg = r.leg;
        }

        if (r.nextCp >= g_course.cps.size()) r.nextCp = 0;
        Checkpoint const& cp = g_course.cps[r.nextCp];
        if (p->GetMapId() != cp.p.map ||
            Dist2d(p->GetPositionX(), p->GetPositionY(), cp.p.x, cp.p.y) > cp.radius)
            return;
        if (r.nextCp == 0)
        {
            // The start/finish line: a lap.
            if (r.lap >= g_course.laps)
            {
                Finish(r, r.bot ? nullptr : p);
                return;
            }
            ++r.lap;
            r.nextCp = 1;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[KARTING] race %u: %s starts lap %u", g_race.id, r.name.c_str(), r.lap);
            if (!r.bot) SendNotice(p, NOTICE_LAP, r.lap == g_course.laps ? 0 : r.lap);
            return;
        }
        r.nextCp = uint8((r.nextCp + 1) % g_course.cps.size());
    }

    uint32 g_lastStatusLog = 0;

    void LogStatus()
    {
        // Every 10 s while racing: where each racer is and what drives it (bots especially).
        for (Racer const& r : g_race.racers)
        {
            Player* p = ObjectAccessor::FindPlayerNotInWorld(r.guid);
            if (!p) continue;
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "[KARTING] status %s: map %u (%.0f, %.0f, %.0f) inWorld %u alive %u kart %u lap %u cp %u leg %u pt %u motion %u%s%s",
                r.name.c_str(), p->GetMapId(), p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(),
                uint32(p->IsInWorld()), uint32(p->IsAlive()), uint32(OnKart(p)), r.lap, r.nextCp, r.leg, r.linePt,
                uint32(p->GetMotionMaster()->GetCurrentMovementGeneratorType()),
                r.finishMs ? " FINISHED" : "", r.dnf ? " DNF" : "");
        }
    }

    // ── items: the engine ──────────────────────────────────────────────────────────────

    Map* ContinentMap(uint32 mapId) { return sMapMgr.FindMap(mapId, 0); }

    GameObject* FindGo(uint32 mapId, ObjectGuid guid)
    {
        Map* m = guid ? ContinentMap(mapId) : nullptr;
        return m ? m->GetGameObject(guid) : nullptr;
    }

    Creature* FindCreature(uint32 mapId, ObjectGuid guid)
    {
        Map* m = guid ? ContinentMap(mapId) : nullptr;
        return m ? m->GetCreature(guid) : nullptr;
    }

    Player* RacerNear(uint32 mapId, float x, float y, float range)
    {
        for (Racer const& r : g_race.racers)
            if (Player* p = ObjectAccessor::FindPlayer(r.guid))
                if (p->IsInWorld() && p->GetMapId() == mapId && Dist2d(p->GetPositionX(), p->GetPositionY(), x, y) < range)
                    return p;
        return nullptr;
    }

    void SendItem(Player* p, Racer& r)
    {
        if (!Capable(p)) return;
        uint32 now = Now();
        uint16 roulette = r.rouletteUntil && r.rouletteUntil > now ? uint16(r.rouletteUntil - now) : 0;
        uint16 golden = r.goldenUntil && r.goldenUntil > now ? uint16(std::min<uint32>(r.goldenUntil - now, 65535)) : 0;
        WorldPacket data(SMSG_SUI_KART, 8);
        data << uint8(WIRE_VERSION) << uint8(KIND_ITEM) << uint8(r.item) << uint8(r.uses) << uint16(roulette) << uint16(golden);
        p->GetSession()->SendPacket(&data);
        r.sentItem = r.item;
        r.sentUses = r.uses;
        r.sentRoulette = roulette > 0;
    }

    bool Shielded(Player* p) { return p->HasAura(SPELL_STAR) || p->HasAura(SPELL_BOO); }

    // MK64's shape: the leaders get defence, the pack gets shells and boosts, the back gets the equalisers.
    uint8 RollItem(Racer const& r)
    {
        uint32 n = std::max<uint32>(2, uint32(g_race.racers.size()));
        float place = r.rank ? float(r.rank - 1) / float(n - 1) : 0.5f;
        struct W { uint8 item; uint32 w; };
        static W const front[] = { {ITEM_BANANA, 25}, {ITEM_BANANA_BUNCH, 5}, {ITEM_GREEN, 25}, {ITEM_TRIPLE_GREEN, 5},
            {ITEM_RED, 10}, {ITEM_FAKE_BOX, 15}, {ITEM_BOO, 5}, {ITEM_MUSHROOM, 10} };
        static W const middle[] = { {ITEM_RED, 25}, {ITEM_TRIPLE_GREEN, 10}, {ITEM_GREEN, 10}, {ITEM_MUSHROOM, 20},
            {ITEM_TRIPLE_MUSHROOM, 10}, {ITEM_BOO, 5}, {ITEM_BANANA, 5}, {ITEM_STAR, 5}, {ITEM_FAKE_BOX, 5},
            {ITEM_TRIPLE_RED, 5} };
        static W const back[] = { {ITEM_STAR, 20}, {ITEM_TRIPLE_RED, 15}, {ITEM_GOLDEN, 15}, {ITEM_TRIPLE_MUSHROOM, 15},
            {ITEM_LIGHTNING, 10}, {ITEM_SPINY, 10}, {ITEM_RED, 10}, {ITEM_BOO, 5} };
        W const* table = place < 0.25f ? front : place < 0.6f ? middle : back;
        size_t count = place < 0.25f ? std::size(front) : place < 0.6f ? std::size(middle) : std::size(back);
        uint32 total = 0;
        for (size_t i = 0; i < count; ++i) total += table[i].w;
        uint32 roll = urand(0, total - 1);
        for (size_t i = 0; i < count; ++i)
        {
            if (roll < table[i].w) return table[i].item;
            roll -= table[i].w;
        }
        return ITEM_BANANA;
    }

    void Hit(Racer& victim, uint32 spell, char const* why)
    {
        Player* p = ObjectAccessor::FindPlayer(victim.guid);
        uint32 now = Now();
        if (!p || !p->IsInWorld() || victim.finishMs || victim.dnf || Shielded(p) ||
            (victim.lastHitAt && WorldTimer::getMSTimeDiff(victim.lastHitAt, now) < HIT_IMMUNE_MS))
            return;
        victim.lastHitAt = now;
        p->CastSpell(p, spell, true);
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[KARTING] race %u: %s hit by %s", g_race.id, victim.name.c_str(), why);
    }

    Racer* RacerAtRank(uint8 rank)
    {
        for (Racer& r : g_race.racers)
            if (r.rank == rank && !r.finishMs && !r.dnf) return &r;
        return nullptr;
    }

    uint16 NearestLinePoint(uint8 leg, float x, float y)
    {
        std::vector<Point> const& line = g_course.lines[leg];
        uint16 best = 0;
        float bestD = 1e9f;
        for (uint16 k = 0; k < line.size(); ++k)
        {
            float d = Dist2d(line[k].x, line[k].y, x, y);
            if (d < bestD) { bestD = d; best = k; }
        }
        return best;
    }

    void FireShell(Racer& r, Player* p, uint8 type, bool backward)
    {
        float o = p->GetOrientation() + (backward ? float(M_PI) : 0.0f);
        float x = p->GetPositionX() + std::cos(o) * 3.0f, y = p->GetPositionY() + std::sin(o) * 3.0f;
        uint32 entry = type == ITEM_SPINY ? NPC_SPINY_SHELL : (type == ITEM_RED ? NPC_RED_SHELL : NPC_GREEN_SHELL);
        Creature* c = p->SummonCreature(entry, x, y, p->GetPositionZ() + 0.5f, o, TEMPSUMMON_TIMED_DESPAWN, 15000);
        if (!c) return;
        Shell s;
        s.type = type;
        s.creature = c->GetObjectGuid();
        s.owner = r.guid;
        s.map = p->GetMapId();
        s.leg = r.leg;
        s.backward = backward;
        s.bornAt = Now();
        s.lifeMs = type == ITEM_SPINY ? 15000 : 9000;
        uint16 nearest = NearestLinePoint(r.leg, x, y);
        std::vector<Point> const& line = g_course.lines[r.leg];
        s.linePt = backward ? uint16(nearest > 0 ? nearest - 1 : 0) : uint16(std::min<size_t>(line.size() - 1, nearest + 1));
        if (type == ITEM_RED && !backward && r.rank > 1)
            if (Racer* ahead = RacerAtRank(uint8(r.rank - 1))) s.target = ahead->guid;
        if (type == ITEM_SPINY)
            if (Racer* leader = RacerAtRank(1)) s.target = leader->guid;
        g_race.shells.push_back(s);
    }

    void DropHazard(Racer& r, Player* p, uint8 type)
    {
        float o = p->GetOrientation();
        float x = p->GetPositionX() - std::cos(o) * 3.5f, y = p->GetPositionY() - std::sin(o) * 3.5f;
        uint32 entry = type == ITEM_FAKE_BOX ? GO_FAKE_BOX : GO_BANANA;
        GameObject* go = p->SummonGameObject(entry, x, y, p->GetPositionZ(), o, 0, 0, std::sin(o / 2), std::cos(o / 2),
            HAZARD_LIFE_MS / 1000, false);
        if (!go) return;
        Hazard h;
        h.type = type;
        h.go = go->GetObjectGuid();
        h.owner = r.guid;
        h.map = p->GetMapId();
        h.x = x; h.y = y;
        h.bornAt = Now();
        g_race.hazards.push_back(h);
    }

    void UseItem(Racer& r, bool backward)
    {
        Player* p = ObjectAccessor::FindPlayer(r.guid);
        uint32 now = Now();
        if (!p || !p->IsInWorld() || r.item == ITEM_NONE || r.finishMs || r.dnf || g_race.phase != Phase::Racing ||
            (r.rouletteUntil && now < r.rouletteUntil))
            return;
        uint8 item = r.item;
        r.lastUseAt = now;
        switch (item)
        {
            case ITEM_BANANA: case ITEM_BANANA_BUNCH: case ITEM_FAKE_BOX:
                DropHazard(r, p, item == ITEM_FAKE_BOX ? ITEM_FAKE_BOX : ITEM_BANANA);
                break;
            case ITEM_GREEN: case ITEM_TRIPLE_GREEN:
                FireShell(r, p, ITEM_GREEN, backward);
                break;
            case ITEM_RED: case ITEM_TRIPLE_RED:
                FireShell(r, p, ITEM_RED, backward);
                break;
            case ITEM_SPINY:
                FireShell(r, p, ITEM_SPINY, false);
                break;
            case ITEM_MUSHROOM: case ITEM_TRIPLE_MUSHROOM:
                p->CastSpell(p, SPELL_BOOST, true);
                break;
            case ITEM_GOLDEN:
                if (!r.goldenUntil) r.goldenUntil = now + GOLDEN_MS;
                p->CastSpell(p, SPELL_BOOST, true);
                SendItem(p, r);
                return;   // the golden mushroom stays until its time is up
            case ITEM_STAR:
                p->CastSpell(p, SPELL_STAR, true);
                break;
            case ITEM_LIGHTNING:
                // Every OTHER racer: shrunk, spun, and its item dropped (a star or a Boo protects).
                for (Racer& o : g_race.racers)
                {
                    if (o.guid == r.guid || o.finishMs || o.dnf) continue;
                    Player* op = ObjectAccessor::FindPlayer(o.guid);
                    if (!op || !op->IsInWorld() || Shielded(op)) continue;
                    op->CastSpell(op, SPELL_SHRUNK, true);
                    o.lastHitAt = 0;
                    Hit(o, SPELL_SPIN, "lightning");
                    o.item = ITEM_NONE;
                    o.uses = 0;
                    o.goldenUntil = 0;
                }
                break;
            case ITEM_BOO:
            {
                p->CastSpell(p, SPELL_BOO, true);
                // ...and steals a random opponent's item.
                std::vector<Racer*> victims;
                for (Racer& o : g_race.racers)
                    if (o.guid != r.guid && o.item != ITEM_NONE && (!o.rouletteUntil || now >= o.rouletteUntil))
                        victims.push_back(&o);
                r.item = ITEM_NONE;
                r.uses = 0;
                if (!victims.empty())
                {
                    Racer* v = victims[urand(0, uint32(victims.size()) - 1)];
                    r.item = v->item;
                    r.uses = v->uses;
                    v->item = ITEM_NONE;
                    v->uses = 0;
                    v->goldenUntil = 0;
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[KARTING] race %u: %s's Boo steals from %s", g_race.id, r.name.c_str(), v->name.c_str());
                }
                if (!r.bot) SendItem(p, r);
                return;
            }
            default:
                break;
        }
        if (r.uses > 0) --r.uses;
        if (r.uses == 0) r.item = ITEM_NONE;
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[KARTING] race %u: %s uses item %u%s", g_race.id, r.name.c_str(), item, backward ? " backwards" : "");
        if (!r.bot) SendItem(p, r);
    }

    void BotItems(Racer& r, Player* p)
    {
        uint32 now = Now();
        if (r.item == ITEM_NONE || (r.rouletteUntil && now < r.rouletteUntil)) return;
        uint32 held = WorldTimer::getMSTimeDiff(r.itemSince, now);
        bool sinceUse = !r.lastUseAt || WorldTimer::getMSTimeDiff(r.lastUseAt, now) > 900;
        auto someone = [&](bool ahead, float range)
        {
            for (Racer const& o : g_race.racers)
            {
                if (o.guid == r.guid || o.finishMs || o.dnf) continue;
                if (ahead ? o.rank >= r.rank : o.rank <= r.rank) continue;
                if (Player* op = ObjectAccessor::FindPlayer(o.guid))
                    if (op->GetMapId() == p->GetMapId() &&
                        Dist2d(op->GetPositionX(), op->GetPositionY(), p->GetPositionX(), p->GetPositionY()) < range)
                        return true;
            }
            return false;
        };
        switch (r.item)
        {
            case ITEM_MUSHROOM: case ITEM_TRIPLE_MUSHROOM: case ITEM_GOLDEN:
                if (sinceUse) UseItem(r, false);
                break;
            case ITEM_STAR: case ITEM_LIGHTNING: case ITEM_BOO: case ITEM_SPINY:
                UseItem(r, false);
                break;
            case ITEM_GREEN: case ITEM_TRIPLE_GREEN: case ITEM_RED: case ITEM_TRIPLE_RED:
                if (sinceUse && (someone(true, 120.0f) || held > 8000)) UseItem(r, false);
                break;
            default:   // bananas, fake box: for whoever is close behind
                if (sinceUse && (someone(false, 40.0f) || held > 10000)) UseItem(r, false);
                break;
        }
    }

    void TickItems()
    {
        uint32 now = Now();

        // Item boxes: summoned when a racer comes near (a summon needs a loaded grid), broken by a
        // drive-through, back after BOX_RESPAWN_MS.
        for (BoxSlot& b : g_race.boxes)
        {
            GameObject* go = FindGo(b.p.map, b.go);
            if (!go)
            {
                b.go.Clear();
                if (now < b.hiddenUntil) continue;
                if (Player* near = RacerNear(b.p.map, b.p.x, b.p.y, 180.0f))
                    if (GameObject* made = near->SummonGameObject(GO_ITEM_BOX, b.p.x, b.p.y, b.p.z + 1.0f, b.p.o, 0, 0,
                            std::sin(b.p.o / 2), std::cos(b.p.o / 2), 900, false))
                        b.go = made->GetObjectGuid();
                continue;
            }
            for (Racer& r : g_race.racers)
            {
                if (r.finishMs || r.dnf) continue;
                Player* p = ObjectAccessor::FindPlayer(r.guid);
                if (!p || !p->IsInWorld() || p->GetMapId() != b.p.map ||
                    Dist2d(p->GetPositionX(), p->GetPositionY(), b.p.x, b.p.y) > BOX_RADIUS)
                    continue;
                go->Delete();
                b.go.Clear();
                b.hiddenUntil = now + BOX_RESPAWN_MS;
                if (r.item == ITEM_NONE)
                {
                    r.item = RollItem(r);
                    r.uses = UsesOf(r.item);
                    r.rouletteUntil = now + ROULETTE_MS;
                    r.itemSince = now;
                    if (!r.bot) SendItem(p, r);
                }
                break;
            }
        }

        // Bananas and fake boxes on the road.
        for (auto it = g_race.hazards.begin(); it != g_race.hazards.end();)
        {
            GameObject* go = FindGo(it->map, it->go);
            bool gone = !go || WorldTimer::getMSTimeDiff(it->bornAt, now) > HAZARD_LIFE_MS;
            if (!gone)
                for (Racer& r : g_race.racers)
                {
                    if (r.guid == it->owner && WorldTimer::getMSTimeDiff(it->bornAt, now) < 1500) continue;
                    Player* p = ObjectAccessor::FindPlayer(r.guid);
                    if (!p || !p->IsInWorld() || p->GetMapId() != it->map ||
                        Dist2d(p->GetPositionX(), p->GetPositionY(), it->x, it->y) > HAZARD_RADIUS)
                        continue;
                    Hit(r, it->type == ITEM_FAKE_BOX ? SPELL_TUMBLE : SPELL_SPIN, it->type == ITEM_FAKE_BOX ? "a fake item box" : "a banana");
                    gone = true;
                    break;
                }
            if (gone)
            {
                if (go) go->Delete();
                it = g_race.hazards.erase(it);
            }
            else
                ++it;
        }

        // Shells: green ones run the racing line, red ones home on the racer ahead, the spiny one hunts the leader.
        for (auto it = g_race.shells.begin(); it != g_race.shells.end();)
        {
            Shell& s = *it;
            Creature* c = FindCreature(s.map, s.creature);
            bool gone = !c || !c->IsAlive() || WorldTimer::getMSTimeDiff(s.bornAt, now) > s.lifeMs;
            if (!gone)
            {
                Player* target = s.target ? ObjectAccessor::FindPlayer(s.target) : nullptr;
                Racer* targetRacer = s.target ? FindRacer(s.target) : nullptr;
                bool homing = target && target->IsInWorld() && target->GetMapId() == s.map && targetRacer &&
                    !targetRacer->finishMs && !targetRacer->dnf;
                if (s.type == ITEM_SPINY && s.target && !homing && targetRacer && !targetRacer->finishMs)
                {
                    // The leader is on another map (through a portal): the spiny shell gets there anyway.
                    g_race.blasts.push_back({ s.target, now + 3000 });
                    gone = true;
                }
                else
                {
                    float speed = s.type == ITEM_SPINY ? SPINY_SPEED : s.type == ITEM_RED ? RED_SPEED : GREEN_SPEED;
                    float tx, ty, tz;
                    if (homing)
                    {
                        tx = target->GetPositionX(); ty = target->GetPositionY(); tz = target->GetPositionZ();
                    }
                    else
                    {
                        std::vector<Point> const& line = g_course.lines[s.leg];
                        Point const& pt = line[std::min<size_t>(s.linePt, line.size() - 1)];
                        if (Dist2d(c->GetPositionX(), c->GetPositionY(), pt.x, pt.y) < 6.0f)
                        {
                            if (s.backward ? s.linePt == 0 : s.linePt + 1 >= line.size()) gone = true;
                            else s.linePt = uint16(s.backward ? s.linePt - 1 : s.linePt + 1);
                        }
                        Point const& next = line[std::min<size_t>(s.linePt, line.size() - 1)];
                        tx = next.x; ty = next.y; tz = next.z;
                    }
                    if (!gone && (WorldTimer::getMSTimeDiff(s.orderedAt, now) > 250 || s.orderedPt != s.linePt))
                    {
                        c->GetMotionMaster()->MovePoint(1, tx, ty, tz, MOVE_PATHFINDING | MOVE_RUN_MODE, speed);
                        s.orderedAt = now;
                        s.orderedPt = s.linePt;
                    }
                    // Contact.
                    for (Racer& r : g_race.racers)
                    {
                        if (gone) break;
                        if (r.guid == s.owner && WorldTimer::getMSTimeDiff(s.bornAt, now) < 1000) continue;
                        Player* p = ObjectAccessor::FindPlayer(r.guid);
                        if (!p || !p->IsInWorld() || p->GetMapId() != s.map ||
                            c->GetDistance2d(p->GetPositionX(), p->GetPositionY()) > SHELL_RADIUS)
                            continue;
                        if (s.type == ITEM_SPINY && r.guid == s.target)
                            g_race.blasts.push_back({ s.target, now });
                        else
                            Hit(r, SPELL_TUMBLE, s.type == ITEM_RED ? "a red shell" : s.type == ITEM_SPINY ? "the spiny shell" : "a green shell");
                        gone = true;
                    }
                }
            }
            if (gone)
            {
                if (c) c->ForcedDespawn();
                it = g_race.shells.erase(it);
            }
            else
                ++it;
        }

        // The spiny shell's blast: the leader and everyone near it.
        for (auto it = g_race.blasts.begin(); it != g_race.blasts.end();)
        {
            if (now < it->at) { ++it; continue; }
            Player* centre = ObjectAccessor::FindPlayer(it->target);
            if (centre && centre->IsInWorld())
                for (Racer& r : g_race.racers)
                {
                    Player* p = ObjectAccessor::FindPlayer(r.guid);
                    if (p && p->IsInWorld() && p->GetMapId() == centre->GetMapId() &&
                        Dist2d(p->GetPositionX(), p->GetPositionY(), centre->GetPositionX(), centre->GetPositionY()) <= SPINY_BLAST_RADIUS)
                        Hit(r, SPELL_TUMBLE, "the spiny shell's blast");
                }
            it = g_race.blasts.erase(it);
        }

        // Star contact, the shrunk squash, the golden mushroom's clock, bots, item state.
        for (Racer& r : g_race.racers)
        {
            Player* p = ObjectAccessor::FindPlayer(r.guid);
            if (!p || !p->IsInWorld() || r.finishMs || r.dnf) continue;
            bool star = p->HasAura(SPELL_STAR);
            bool small = p->HasAura(SPELL_SHRUNK);
            if (star || !small)
                for (Racer& o : g_race.racers)
                {
                    if (o.guid == r.guid || o.finishMs || o.dnf) continue;
                    Player* op = ObjectAccessor::FindPlayer(o.guid);
                    if (!op || !op->IsInWorld() || op->GetMapId() != p->GetMapId() ||
                        Dist2d(op->GetPositionX(), op->GetPositionY(), p->GetPositionX(), p->GetPositionY()) > STAR_RADIUS)
                        continue;
                    if (star) Hit(o, SPELL_SPIN, "a star");
                    else if (op->HasAura(SPELL_SHRUNK)) Hit(o, SPELL_TUMBLE, "being squashed");
                }
            if (r.goldenUntil && now >= r.goldenUntil)
            {
                r.goldenUntil = 0;
                r.item = ITEM_NONE;
                r.uses = 0;
            }
            if (r.bot)
                BotItems(r, p);
            else if (r.sentItem != r.item || r.sentUses != r.uses || (r.sentRoulette && now >= r.rouletteUntil))
                SendItem(p, r);
        }
    }

    void ClearItems()
    {
        for (BoxSlot& b : g_race.boxes)
            if (GameObject* go = FindGo(b.p.map, b.go)) go->Delete();
        for (Hazard& h : g_race.hazards)
            if (GameObject* go = FindGo(h.map, h.go)) go->Delete();
        for (Shell& s : g_race.shells)
            if (Creature* c = FindCreature(s.map, s.creature)) c->ForcedDespawn();
        g_race.boxes.clear();
        g_race.hazards.clear();
        g_race.shells.clear();
        g_race.blasts.clear();
    }

    void TickRacing()
    {
        uint32 now = Now();
        if (WorldTimer::getMSTimeDiff(g_lastStatusLog, now) >= 10000) { g_lastStatusLog = now; LogStatus(); }
        for (Racer& r : g_race.racers) UpdateRacer(r);
        Rank();
        TickItems();
        PushAll(false);

        bool humansDone = true;
        bool anyHuman = false;
        for (Racer const& r : g_race.racers)
            if (!r.bot)
            {
                anyHuman = true;
                humansDone &= r.finishMs != 0 || r.dnf;
            }
        bool allDone = std::all_of(g_race.racers.begin(), g_race.racers.end(), [](Racer const& r) { return r.finishMs || r.dnf; });
        bool graceOver = g_race.firstFinishAt && WorldTimer::getMSTimeDiff(g_race.firstFinishAt, now) >= FINISH_GRACE_MS;
        bool timedOut = WorldTimer::getMSTimeDiff(g_race.goAt, now) >= RACE_TIMEOUT_MS;
        // The field gets to finish (MK64): the race ends when everyone is done, FINISH_GRACE_MS after the winner,
        // on the timeout, or when no human is left racing.
        bool humansRacing = anyHuman && !humansDone;
        if (allDone || graceOver || timedOut || !anyHuman || (!humansRacing && !g_race.finishedCount && humansDone))
        {
            for (Racer& r : g_race.racers)
                if (!r.finishMs) r.dnf = true;
            Rank();
            g_race.phase = Phase::Finished;
            g_race.phaseAt = now;
            PushAll(true);
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[KARTING] race %u finished", g_race.id);
        }
    }

    void StartRace()
    {
        g_race.id = g_nextRaceId++;
        g_race.phase = Phase::Gathering;
        g_race.phaseAt = Now();
    }

    bool Join(Player* p)
    {
        if (!g_course.loaded) { SendNotice(p, NOTICE_NO_COURSE); return false; }
        if (FindRacer(p->GetObjectGuid())) { PushAll(true); return true; }
        if (g_race.phase == Phase::Countdown || g_race.phase == Phase::Racing || g_race.racers.size() >= MAX_RACERS)
        {
            SendNotice(p, NOTICE_FULL);
            return false;
        }
        if (g_race.phase == Phase::Finished) ResetRace();
        if (g_race.phase == Phase::Idle) StartRace();
        Racer r;
        r.guid = p->GetObjectGuid();
        r.name = p->GetName();
        // Keep the kart the racer chose at the marshal; anyone else gets the go-kart.
        r.kart = !SuiKarting::IsNeutralKartRider(p) ? KART_RACING : p->HasAura(KART_GNOME) ? KART_GNOME
            : p->HasAura(KART_GOBLIN) ? KART_GOBLIN : KART_RACING;
        g_race.racers.push_back(r);
        Mount(p, r.kart);
        SendNotice(p, NOTICE_JOINED);
        PushAll(true);
        return true;
    }

    void Leave(Player* p)
    {
        auto it = std::find_if(g_race.racers.begin(), g_race.racers.end(), [&](Racer const& r) { return r.guid == p->GetObjectGuid(); });
        if (it == g_race.racers.end()) return;
        p->SetRooted(false);
        g_race.racers.erase(it);
        SendNotice(p, NOTICE_LEFT);
        SendState(p, nullptr);
    }

    void RequestStart(Player* p)
    {
        if (!Join(p)) return;
        if (g_race.phase != Phase::Gathering) return;
        g_race.startRequested = true;
        uint8 humans = uint8(std::count_if(g_race.racers.begin(), g_race.racers.end(), [](Racer const& r) { return !r.bot; }));
        if (humans < FILL_TO) SpawnBots(uint8(FILL_TO - humans));
        g_race.phaseAt = Now();   // FILL_GATHER_MS from now
    }
}

// ── public API ────────────────────────────────────────────────────────────

void SuiKarting::Tick(uint32 /*diff*/)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_course.attempted) LoadCourse();
    if (!g_course.loaded || g_race.phase == Phase::Idle) return;
    uint32 now = Now();
    uint32 inPhase = WorldTimer::getMSTimeDiff(g_race.phaseAt, now);

    // A human who logged out has left.
    g_race.racers.erase(std::remove_if(g_race.racers.begin(), g_race.racers.end(),
        [](Racer const& r) { return !r.bot && g_race.phase != Phase::Finished && !ObjectAccessor::FindPlayerNotInWorld(r.guid); }),
        g_race.racers.end());

    switch (g_race.phase)
    {
        case Phase::Gathering:
            AdoptPendingBots();
            if (g_race.racers.empty() && g_race.pendingBots.empty()) { ResetRace(); return; }
            PushAll(false);
            if (inPhase >= (g_race.startRequested ? FILL_GATHER_MS : GATHER_MS))
            {
                if (!g_race.startRequested)
                {
                    uint8 humans = uint8(std::count_if(g_race.racers.begin(), g_race.racers.end(), [](Racer const& r) { return !r.bot; }));
                    if (humans < FILL_TO) SpawnBots(uint8(FILL_TO - humans));
                    g_race.startRequested = true;
                    g_race.phaseAt = now;
                    return;
                }
                BeginCountdown();
            }
            break;
        case Phase::Countdown:
        {
            // 3, 2, 1, GO on the beat after GRID_LEAD_MS on the grid.
            if (inPhase < GRID_LEAD_MS) { PushAll(false); return; }
            int8 beat = int8(3 - int32((inPhase - GRID_LEAD_MS) / BEAT_MS));
            if (beat < 0) beat = 0;
            if (beat != g_race.beat)
            {
                g_race.beat = beat;
                for (Racer const& r : g_race.racers)
                    if (!r.bot)
                        if (Player* p = ObjectAccessor::FindPlayer(r.guid)) SendBeat(p, uint8(beat));
                char const* words[] = { "GO!", "One!", "Two!", "Three!" };
                Yell(words[beat]);
                if (beat == 0) Go();
            }
            break;
        }
        case Phase::Racing:
            TickRacing();
            break;
        case Phase::Finished:
            PushAll(false);
            if (inPhase >= RESULTS_MS) ResetRace();
            break;
        default:
            break;
    }
}

bool SuiKarting::TickRacer(AiBotAI* ai, uint32 /*diff*/)
{
    Player* me = ai->GetBotPlayer();
    if (!me) return false;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_race.phase == Phase::Idle) return false;
    Racer* r = FindRacer(me->GetObjectGuid());
    if (!r) return false;
    if (!me->IsInWorld() || me->IsBeingTeleported() || !me->IsAlive()) return true;

    auto hold = [&]()
    {
        // A server spline sets no movement flag: stop once per hold, not by polling IsMoving.
        if (!r->held) { me->StopMoving(); me->GetMotionMaster()->MoveIdle(); r->held = true; }
        r->orderedPt = 0xFFFF;
        return true;
    };
    if (g_race.phase != Phase::Racing || r->finishMs || r->dnf || !SuiKarting::IsNeutralKartRider(me) ||
        me->HasUnitState(UNIT_STATE_STUNNED))
        return hold();
    if (r->leg >= g_course.lines.size()) return hold();
    std::vector<Point> const& line = g_course.lines[r->leg];
    if (line.empty()) return hold();
    if (r->linePt >= line.size()) r->linePt = uint16(line.size() - 1);
    Point const& target = line[r->linePt];
    if (me->GetMapId() != target.map) return true;   // between maps

    float d = Dist2d(me->GetPositionX(), me->GetPositionY(), target.x, target.y);
    bool last = r->linePt + 1 == line.size();
    if (last)
    {
        // The leg's portal: use it for real, exactly as a driver's client does.
        uint32 entry = r->leg < g_course.portalEntry.size() ? g_course.portalEntry[r->leg] : 0;
        if (entry && d < 12.0f)
            if (GameObject* go = me->FindNearestGameObject(entry, 15.0f))
                // Interact distance OR 12 yd: a portal hung off a ledge (the Stormwind wall exit) is a few yards out
                // and down from the last point a bot can stand on (2026-10-04: every bot stalled at the wall edge).
                if ((go->IsAtInteractDistance(me) || me->GetDistance(go) < 12.0f) && go->PlayerCanUse(me))
                {
                    me->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_LOOTING_CANCELS);
                    go->Use(me);
                    bool viaUse = me->IsBeingTeleported();
                    bool ok = viaUse;
                    if (!viaUse)
                    {
                        // The portal's own spell did not move this bot (2026-10-04 live: Use left every racer bot
                        // standing at the lip): teleport it to the very same destination the spell would.
                        GameObjectInfo const* info = go->GetGOInfo();
                        SpellTargetPosition const* st = info ? sSpellMgr.GetSpellTargetPosition(info->spellcaster.spellId) : nullptr;
                        ok = st && me->TeleportTo(st->mapId, st->x, st->y, st->z, st->o, TELE_TO_NOT_LEAVE_COMBAT | TELE_TO_NOT_UNSUMMON_PET);
                    }
                    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "[KARTING] race %u: %s takes portal %u (%s, %s)", g_race.id,
                        r->name.c_str(), entry, viaUse ? "gameobject use" : "direct teleport", ok ? "teleporting" : "FAILED");
                    if (!ok) return true;
                    r->leg = uint8((r->leg + 1) % g_course.lines.size());
                    r->linePt = 1;
                    r->orderedPt = 0xFFFF;
                    return true;
                }
    }
    else if (d < 10.0f)
    {
        ++r->linePt;
        r->orderedPt = 0xFFFF;
        return true;
    }

    uint32 now = Now();
    float speed = me->GetSpeed(MOVE_RUN) * r->skill;
    if (r->held || r->orderedPt != r->linePt || r->orderedLeg != r->leg || std::fabs(speed - r->orderedSpeed) > 0.5f ||
        WorldTimer::getMSTimeDiff(r->lastOrderAt, now) > 1500)
    {
        r->held = false;
        r->orderedSpeed = speed;
        me->GetMotionMaster()->MovePoint(BOT_POINT_ID, target.x, target.y, target.z, MOVE_PATHFINDING | MOVE_RUN_MODE, speed);
        r->orderedPt = r->linePt;
        r->orderedLeg = r->leg;
        r->lastOrderAt = now;
    }
    return true;
}

void SuiKarting::Shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_race = Race();
}

// ── CMSG_SUI_KART ─────────────────────────────────────────────────────────

void WorldSession::HandleSuiKartOpcode(WorldPackets::SuiControl::Kart const& packet)
{
    SetSuiCapable(true);
    if (!packet.exactSize || GetBot()) return;
    Player* p = GetPlayer();
    if (!p || !p->IsInWorld()) return;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_course.attempted) LoadCourse();
    switch (packet.action)
    {
        case ACTION_JOIN: Join(p); break;
        case ACTION_LEAVE: Leave(p); break;
        case ACTION_START: RequestStart(p); break;
        case ACTION_USE:
            if (Racer* r = FindRacer(p->GetObjectGuid())) UseItem(*r, packet.param != 0);
            break;
        case ACTION_QUERY:
            if (!g_course.loaded) SendNotice(p, NOTICE_NO_COURSE);
            else SendState(p, FindRacer(p->GetObjectGuid()));
            break;
        default: break;
    }
}

// ── the race marshal (creature_template.script_name = npc_sui_kart_marshal) ──

namespace
{
    enum MarshalAction : uint32
    {
        MA_GOBLIN = GOSSIP_ACTION_INFO_DEF + 1,
        MA_GNOME = GOSSIP_ACTION_INFO_DEF + 2,
        MA_JOIN = GOSSIP_ACTION_INFO_DEF + 3,
        MA_START = GOSSIP_ACTION_INFO_DEF + 4,
        MA_LEAVE = GOSSIP_ACTION_INFO_DEF + 5,
        MA_RACING = GOSSIP_ACTION_INFO_DEF + 6,
    };

    bool GossipHello_KartMarshal(Player* p, Creature* c)
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (!g_course.attempted) LoadCourse();
        p->ADD_GOSSIP_ITEM(0, "I'd like a Racing Kart.", GOSSIP_SENDER_MAIN, MA_RACING);
        p->ADD_GOSSIP_ITEM(0, "I'd like a Goblin Racer kart.", GOSSIP_SENDER_MAIN, MA_GOBLIN);
        p->ADD_GOSSIP_ITEM(0, "I'd like a Gnome Racer kart.", GOSSIP_SENDER_MAIN, MA_GNOME);
        if (g_course.loaded)
        {
            if (FindRacer(p->GetObjectGuid()))
                p->ADD_GOSSIP_ITEM(0, "Count me out of this race.", GOSSIP_SENDER_MAIN, MA_LEAVE);
            else
                p->ADD_GOSSIP_ITEM(0, "Sign me up for the next race.", GOSSIP_SENDER_MAIN, MA_JOIN);
            p->ADD_GOSSIP_ITEM(0, "Start the race now - fill the grid with racers!", GOSSIP_SENDER_MAIN, MA_START);
        }
        p->SEND_GOSSIP_MENU(MARSHAL_TEXT, c->GetGUID());
        return true;
    }

    bool GossipSelect_KartMarshal(Player* p, Creature* /*c*/, uint32 /*sender*/, uint32 action)
    {
        p->CLOSE_GOSSIP_MENU();
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        switch (action)
        {
            case MA_GOBLIN:
                if (p->IsMounted()) p->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
                p->CastSpell(p, KART_GOBLIN, true);
                break;
            case MA_GNOME:
                if (p->IsMounted()) p->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
                p->CastSpell(p, KART_GNOME, true);
                break;
            case MA_RACING:
                if (p->IsMounted()) p->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
                p->CastSpell(p, KART_RACING, true);
                break;
            case MA_JOIN: Join(p); break;
            case MA_START: RequestStart(p); break;
            case MA_LEAVE: Leave(p); break;
            default: break;
        }
        return true;
    }
}

void AddSC_sui_karting()
{
    Script* s = new Script;
    s->Name = "npc_sui_kart_marshal";
    s->pGossipHello = &GossipHello_KartMarshal;
    s->pGossipSelect = &GossipSelect_KartMarshal;
    s->RegisterSelf(false);
}
