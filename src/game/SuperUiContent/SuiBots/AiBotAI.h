/*
 * AiBotAI.h — compatibility shim.
 *
 * The AiBotAI class declaration moved to AiBotAIMain.h when AiBotAI.cpp was split
 * into per-concern translation units (Main / Combat / Bridge / Movement / Loot / Grind).
 * This one-line forwarder keeps every existing include site working unchanged
 * (AiBotAITeamPlay.cpp/.h, PlayerBotMgr, the bot factory) — they still `#include "AiBotAI.h"`.
 *
 * New code should include "AiBotAIMain.h" directly.
 */

#ifndef MANGOS_AIBOTAI_SHIM_H
#define MANGOS_AIBOTAI_SHIM_H

#include "AiBotAIMain.h"

#endif
