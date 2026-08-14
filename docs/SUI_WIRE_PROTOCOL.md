# SUI Wire Protocol — CRPG/RTS Control Extension

Custom opcodes between **MSUIClient** and **SuperUI-Core**, layered on the stock
1.12 (build 5875) world protocol. A vanilla client never emits opcodes above
827, and the custom SMSGs are only sent to sessions that have spoken a
`CMSG_SUI_*` first (`WorldSession::IsSuiCapable`), so stock clients never see
them. All integers little-endian; guids are raw `uint64`, never packed.

Server implementation: `src/game/SuperUiBots/SuiPossess.{h,cpp}` and
`src/game/SuperUiBots/SuiPortal.{h,cpp}`, packet structs in
`src/game/Server/Packets/SuiControl.h` and `SuiPortalPackets.h`.
Client implementation: MSUIClient `Program.Control.cs` / `Net/`.

| Opcode | Value | Direction |
|---|---|---|
| `CMSG_SUI_CONTROL_REQUEST` | 828 (0x033C) | C → S |
| `CMSG_SUI_CONTROL_RELEASE` | 829 (0x033D) | C → S |
| `CMSG_SUI_ORDER`           | 830 (0x033E) | C → S |
| `SMSG_SUI_CONTROL_ROSTER`  | 831 (0x033F) | S → C |
| `SMSG_SUI_CONTROL_ACK`     | 832 (0x0340) | S → C |
| `SMSG_SUI_PROXY`           | 833 (0x0341) | S → C |
| `SMSG_SUI_SNAPSHOT`        | 834 (0x0342) | S → C |
| `CMSG_SUI_CAM`             | 835 (0x0343) | C → S |
| `CMSG_SUI_ZONE_INTEL`      | 836 (0x0344) | C → S |
| `SMSG_SUI_ZONE_INTEL`      | 837 (0x0345) | S → C |
| `CMSG_SUI_RTS_STATE`       | 838 (0x0346) | C → S |
| `SMSG_SUI_RTS_STATE`       | 839 (0x0347) | S → C |
| `CMSG_SUI_RTS_ACTION`      | 840 (0x0348) | C → S |
| `SMSG_SUI_RTS_ACTION_RESULT` | 841 (0x0349) | S → C |

### Real-portal opcode block

| Opcode | Value | Direction |
|---|---|---|
| `CMSG_SUI_PORTAL_PREPARE`  | 844 (0x034C) | C -> S |
| `SMSG_SUI_PORTAL_DESCRIPTOR` | 845 (0x034D) | S -> C |
| `CMSG_SUI_PORTAL_READY`    | 846 (0x034E) | C -> S |
| `SMSG_SUI_PORTAL_STATE`    | 847 (0x034F) | S -> C |

Values 835-843 are reserved for the camera, zone-intel, and RTS extensions.
The portal values are intentionally fixed above that range so those branches
can merge without renumbering wire traffic. `NUM_MSG_TYPES` is 848.

## CMSG_SUI_CONTROL_REQUEST
Ask to possess a party/raid bot.

| Field | Type | Notes |
|---|---|---|
| targetGuid | u64 | the bot to possess |

Server validation (deny codes in parentheses): requester is a real session and
self-mover, alive, not on taxi/transport, possessing nothing (6); target exists,
in world, same map+instance, visible (1); target session is a bot with AiBotAI
(2); same group (3); not already possessed (4); target alive, not on
taxi/transport, not teleporting (5).

On grant, in order: bot AI suspended (`AiBotAI::m_possessed`, bridge commands
gated, brain sees `possessed:1` STATE), bot loot force-released,
`Camera::SetView(bot)`, `SetPossessorGuid`, `SetMover(bot)`,
`SetClientControl(bot, 1)`, then `SMSG_SUI_CONTROL_ACK(OK)`.
**The client must answer the grant with `CMSG_SET_ACTIVE_MOVER(botGuid)`** —
guaranteed to be accepted because the server set the mover first — then stream
`MSG_MOVE_*` from the ACK position.

## CMSG_SUI_CONTROL_RELEASE
| Field | Type | Notes |
|---|---|---|
| mode | u8 | 0 = back to own character, 1 = free view (own char stays autonomous) |

Safe to send when possessing nothing (freecam enter/leave from the own
character); always answered with an ACK. The client should flush a
`MSG_MOVE_STOP` at the bot's position **before** sending this, then park its
movement stream until the ACK.

## CMSG_SUI_ORDER (M6)
| Field | Type | Notes |
|---|---|---|
| orderType | u8 | 0 move, 1 attack, 2 stop |
| subjectCount | u8 | 0 = every controllable bot in the group |
| subjects | u64 × count | |
| targetGuid | u64 | attack orders; 0 otherwise |
| x, y, z | f32 × 3 | move orders |

## SMSG_SUI_CONTROL_ROSTER
Pushed on every group roster change (`Group::SendUpdate`) and on possession
begin/end. Lists **all** current group members.

| Field | Type | Notes |
|---|---|---|
| count | u8 | |
| per member: guid | u64 | |
| per member: flags | u8 | 0x01 controllable (AiBot), 0x02 currently possessed |

## SMSG_SUI_CONTROL_ACK
Answers requests/releases AND arrives **unsolicited** on forced release — the
client must accept it in any control state.

| Field | Type | Notes |
|---|---|---|
| guid | u64 | granted bot, or own character on release/deny |
| result | u8 | see below |
| x, y, z, o | f32 × 4 | authoritative position of the unit the client now drives |

Results: 0 OK · 1 DENY_NOT_FOUND · 2 DENY_NOT_BOT · 3 DENY_NOT_IN_GROUP ·
4 DENY_BUSY · 5 DENY_TARGET_STATE · 6 DENY_REQUESTER_STATE ·
16 RELEASED · 17 RELEASED_FREECAM · 18 RELEASED_DEATH · 19 RELEASED_TELEPORT ·
20 RELEASED_GROUP · 21 RELEASED_LOGOUT

On any release result the client teleports its controller to the carried
position (the own character may have moved under its AI). Server-initiated
releases open a 1 s movement drain window on the session so in-flight
bot-coordinate `MSG_MOVE_*` are discarded, not misattributed.

The fixed ACK prefix remains 25 bytes. Current cores append an optional
eight-byte capability trailer: `u32 0x31495553` (`SUI1` on the little-endian
wire), then a `u32` capability mask. Bit 0 advertises REAL_PORTALS version 1;
bit 1 advertises the server-authored cast-prewarm catalog described below.
Older clients may ignore the suffix and any catalog bytes after it. A current
client probes safely with a zero-guid `CMSG_SUI_CONTROL_REQUEST`: an older core
returns its ordinary denial without a trailer, while a portal-capable core
advertises bit 0. The client must not send portal opcodes until that bit is
observed.

When bit 1 is present, the mask is followed by `u8 catalogVersion = 1`, `u8
rowCount = 6`, `u16 rowBytes = 32`, then six fixed-stride rows. Each row is
`u32 summonSpellId`, `u32 portalEntry`, `u32 teleportSpellId`, `u32 previewMapId`,
and `f32 previewX, previewY, previewZ, previewOrientation`. The rows cover only
Mage portal summon spells 10059 and 11416-11420. Destinations are resolved from
the live server `spell_target_position` table; the client never hard-codes city
coordinates. A new client may begin cancellable destination-only work when its
own `SMSG_SPELL_START` matches a row. This hint has no GameObject GUID, ticket,
lease, use permission, or teleport authority: visual publication and READY still
require the later ordinary portal descriptor for the exact spawned object.

## SMSG_SUI_PROXY (M3)
Owner-only packets of the possessed bot, re-wrapped into the possessor's
session (the bot's session is socket-less; its `SendPacket` also forwards a
whitelist here — never *instead of* the bot AI's own `OnPacketReceived`).

| Field | Type | Notes |
|---|---|---|
| sourceGuid | u64 | the bot the inner packet describes |
| innerOpcode | u16 | e.g. SMSG_ACTION_BUTTONS, SMSG_INITIAL_SPELLS, SMSG_SPELL_COOLDOWN, SMSG_CAST_RESULT |
| innerBody | rest | verbatim inner packet body |

The client routes the inner body through its normal parser into the per-guid
store and **drops proxies whose sourceGuid ≠ its current controlled guid**
(possession-boundary stragglers).

## SMSG_SUI_SNAPSHOT (M4)
Read-only bags/talents for the possessed bot, pushed once after a grant.
Layout finalized in M4.

## Bridge STATE addition
`AiBotAI::BridgeSendState` gained `"possessed": 0|1`. The C# brain must hold
all planners/driver ticks for a possessed bot (mirror of the `pparty`
stand-down). Mutating bridge commands sent anyway are dropped with an
`EVENT { event: "POSSESSED_DROP", data: "<command>" }`.

## GM commands (stock-client testable path)
`.sui possess <botname>` (or with the bot selected) and `.sui release` drive
the same core without any custom SMSGs — control still works because the grant
uses stock `SMSG_CLIENT_CONTROL_UPDATE` + `CMSG_SET_ACTIVE_MOVER`. This makes
the whole server milestone testable from an unmodified 1.12 client.

## CMSG_SUI_CAM

Free-view camera position; the server keeps the streaming eye under it
(heartbeat: every 2 s or >5 yd of rig travel). The trailing ACTIVE byte is the
free view’s on/off signal — optional on the wire, absent reads as “up”.

| Field | Type | Notes |
|---|---|---|
| x, y, z | 3×f32 | rig position, raw map coords |
| active | u8 (optional) | 0 = the free view came down |

## CMSG_SUI_ZONE_INTEL

The commander map’s census request. Client-driven polling: sent on map open and
every ~5 s while it stays open. Marks the session SuiCapable.

| Field | Type | Notes |
|---|---|---|
| flags | u8 (optional) | reserved, send 0 |

## SMSG_SUI_ZONE_INTEL

Answered only to the asker. Two blocks, each with an explicit ROW STRIDE so a
future server can append per-row facts while an older client skips the extra
bytes (`skip(stride - known)`) — the classic optional-trailing idiom cannot
express “every row grew”. Per-PACKET additions still go after the last block.

| Field | Type | Notes |
|---|---|---|
| zoneCount | u16 | sparse: only zones with a nonzero census, all maps |
| zoneRowBytes | u8 | row stride, currently 9 (R1) |
| zoneCount × rows: | | |
|  zoneId | u32 | `Player::GetCachedZoneId` (≤ one zone-tick stale) |
|  bots | u16 | sessions with a `PlayerBotEntry` (`Player::IsBot`) |
|  players | u16 | everyone else — unattended real characters and the asker included |
|  controller | u8 | zone controller: 0 none / 1 alliance / 2 horde, bit 0x80 = contested (always 0 until territory R3) |
| unitCount | u8 | the asker’s own forces: self + in-world group members |
| unitRowBytes | u8 | row stride, currently 29 |
| unitCount × rows: | | |
|  guid | u64 | raw |
|  mapId | u32 | |
|  zoneId | u32 | |
|  x, y, z | 3×f32 | live position (the client cannot see unstreamed members) |
|  flags | u8 | bit0 alive, bit1 bot |

Notes: GM-invisible characters are counted (no filtering in v1). Zone ids the
client has no WorldMapArea entry for (dungeons, raids) appear in the census and
are aggregated by the client as “elsewhere”.

## CMSG_SUI_RTS_STATE / SMSG_SUI_RTS_STATE

Tier-2 RTS worldstate snapshot; answered only to the asker, marks the session
SuiCapable. Client polls on the commander-map cadence (~5 s). In the vanilla
worldstate the reply is mode=0, moduleFlags=0 and all-zero blocks.

Request: `u8 flags` (optional, reserved).

Reply (all blocks stride-versioned — future servers grow rows, old clients skip):

| Field | Type | Notes |
|---|---|---|
| mode | u8 | 0 vanilla, 1 RTS match |
| moduleFlags | u8 | bit0 honor, bit1 heroes, bit2 territory, bit3 dungeons |
| factionRowStride | u8 | currently 26; ALWAYS 2 rows (alliance, horde) |
|  honorPool | i64 | faction honor pool (R2 feeds it) |
|  ore, skins, herbs | 3×i32 | standing supply from held zones (R3) |
|  controlledZones | u16 | R3 |
|  heroesFielded, heroSlotCap | 2×u16 | R2/R3 |
| heroCount, heroRowStride(12) | 2×u8 | rows: u64 guid, u8 team, u8 heroLevel, u8 dead, u8 pad |
| dungeonCount, dungeonRowStride(7) | 2×u8 | rows: u32 mapId, u8 controller, u8 liveRunFlags(bit0 A run, bit1 H run), u8 pad |

## CMSG_SUI_RTS_ACTION / SMSG_SUI_RTS_ACTION_RESULT

Request: `u8 action` (1 heroDeclare, 2 heroUpgrade, 3 heroRevive), `u64 subjectGuid`.
Result: `u8 action, u8 result, u64 subjectGuid, i64 poolAfter`.
Result codes: 0 ok, 1 insufficient honor, 2 no free slot, 3 bad subject,
4 unsupported/disabled (R1 always answers 4; R2 implements).

## RTS ruleset key list (shared with the SuperUI web app registry)

Scalars in `characters.superui_worldstate` (key/value), read ONCE at boot by
`SuiRts::LoadRuleset()`. Absent key = default. This list is the single source
both the core and the web app's RtsRulesetRegistry mirror.

| Key | Default | Phase | Meaning |
|---|---|---|---|
| mode | (absent = vanilla) | — | `rts` flips the worldstate |
| state.flush_ms | 30000 | R1 | write-behind cadence for faction state |
| rate.xp_kill / rate.xp_kill_elite / rate.xp_quest | conf | R1 | sWorld rate overrides |
| rate.drop_money / rate.drop_item_poor..artifact / rate.drop_item_referenced | conf | R1 | sWorld rate overrides |
| bots.cap.alliance / bots.cap.horde | -1 (uncapped) | R1 | per-faction bot population cap |
| honor.weight.player / .bot / .npc / .npc_elite | 10 / 5 / 1 / 3 | R2 | honor-pool kill weights; ANY key present enables the honor module |
| honor.suppress_bot_hk | 1 | R2 | skip vanilla HK recording for bot-vs-bot |
| hero.slots_fixed | 4 | R2 | hero cap until territory lands |
| territory.zones_per_hero_slot | 2 | R3 | AoE-housing slot ratio |
| territory.flip_cooldown_ms | 30000 | R3 | debounce hub re-flips |
| dungeon.run_timeout_min | 120 | R4 | stale live-run safety net |

List config tables (rows ship inside the RTS save): `superui_rules_zone`,
`superui_rules_hub`, `superui_rules_hero`, `superui_rules_dungeon`. Runtime
state: `superui_faction`, `superui_heroes`, `superui_zone_control`,
`superui_dungeon_control`. All DDL is core-owned and idempotent.
## Real portal preparation/readiness (version 1)

This side channel lets each live MSUIClient session preload a destination and
render it only after a complete frame is available. It does not invoke
`GameObject::Use`, authorize a crossing, or replace the stock click handler.
Every client packet first marks the session SUI-capable; no portal SMSG is sent
to a session which has not opted in with a `CMSG_SUI_*`.

All four payloads are fixed-size. Reserved fields and trailing bytes must be
zero/absent. Unknown versions are rejected rather than guessed.

### CMSG_SUI_PORTAL_PREPARE (16 bytes)

| Field | Type | Notes |
|---|---|---|
| version | u8 | 1 |
| reserved | u8 | 0 |
| flags | u16 | 0 in version 1 |
| requestId | u32 | echoed in the descriptor |
| portalGuid | u64 | raw game-object guid |

The server validates that the object is spawned, visible for use, inside the
150-yard preparation radius, has no `GO_FLAG_NO_INTERACT`, passes
`PlayerCanUse`, and passes the spellcaster party-only rule. The authoritative
classifier accepts only type-22 entries 176296, 176497, 176498, 176499, 176500,
and 176501 with their matching `gameobject_template.data0` use spells 17334,
17607, 17608, 17609, 17610, and 17611. Destination coordinates come from
`SpellMgr::GetSpellTargetPosition`; client-side portal-name inference is never
trusted.

The six matching summoned GameObjects are forced into the core's bounded
200-yard large-object visibility tier, so a client can actually discover an
existing portal before the 150-yard preparation boundary. This does not change
the stock click/use distance.

### SMSG_SUI_PORTAL_DESCRIPTOR (92 bytes)

| Field | Type | Notes |
|---|---|---|
| version, result | u8, u8 | version 1; result below |
| flags | u16 | bit 0 ONE_WAY, bit 1 PARTY_ONLY, bit 2 CLICK_FALLBACK, bit 3 SAME_MAP_HINT; bit 4 BIDIRECTIONAL is reserved |
| requestId | u32 | correlation with PREPARE |
| portalGuid | u64 | raw game-object guid |
| generation, revision | u32, u32 | per-session generation; descriptor revision |
| ticket | u64 | readiness correlation only; not teleport authority |
| entry, teleportSpellId | u32, u32 | authoritative classifier inputs |
| remainingLifetimeMs | u32 | `0xFFFFFFFF` means no known expiry |
| sourceX, sourceY, sourceZ, sourceYaw | f32 x 4 | source frame center and facing |
| halfWidth, halfHeight | f32, f32 | 3.0 and 4.0: a 6 x 8 yard aperture |
| crossingEpsilon | f32 | 0.35 yard geometry tolerance |
| destinationMapId | u32 | preview map |
| destinationX, destinationY, destinationZ, destinationO | f32 x 4 | preview pose |

Descriptor results: 0 OK, 1 Denied, 2 Unsupported, 3 Expired, 4 Failed.
Denied/error replies keep the same 92-byte layout and zero fields which are not
valid, so parsing never depends on a result-specific packet length.

### CMSG_SUI_PORTAL_READY (28 bytes)

| Field | Type | Notes |
|---|---|---|
| version, loadResult | u8, u8 | version 1; 0 Ready, 1 Failed |
| reserved | u16 | 0 |
| portalGuid | u64 | descriptor key |
| generation, revision | u32, u32 | descriptor key |
| ticket | u64 | descriptor key |

The server rejects stale/mismatched keys, expired preparation leases, despawned
objects, range changes, and lost use/party eligibility. It re-runs validation;
a client assertion is never treated as proof.

### SMSG_SUI_PORTAL_STATE (32 bytes)

| Field | Type | Notes |
|---|---|---|
| version, state, reason, reserved | u8 x 4 | reserved is 0 |
| portalGuid | u64 | correlated object |
| generation, revision | u32, u32 | correlated descriptor |
| ticket | u64 | correlated descriptor |
| leaseMs | u32 | nonzero only when Ready |

States: 0 Ready, 1 Revoked, 2 Blocked, 3 Entering, 4 Expired, 5 Failed.
Version 1 grants a 5-second Ready lease. A portal that remains open is renewed
by another PREPARE/DESCRIPTOR/READY cycle; the generation/ticket may be reused
while the previous correlation is still live. This lease still does not bypass
the stock `CMSG_GAMEOBJ_USE` range and eligibility checks.
