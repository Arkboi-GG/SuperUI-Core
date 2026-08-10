# SUI Wire Protocol — CRPG/RTS Control Extension

Custom opcodes between **MSUIClient** and **SuperUI-Core**, layered on the stock
1.12 (build 5875) world protocol. A vanilla client never emits opcodes above
827, and the custom SMSGs are only sent to sessions that have spoken a
`CMSG_SUI_*` first (`WorldSession::IsSuiCapable`), so stock clients never see
them. All integers little-endian; guids are raw `uint64`, never packed.

Server implementation: `src/game/SuperUiBots/SuiPossess.{h,cpp}`,
packet structs in `src/game/Server/Packets/SuiControl.h`.
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
