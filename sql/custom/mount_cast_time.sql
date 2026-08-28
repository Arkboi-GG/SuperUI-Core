-- Reduces every player mount spell's cast time to 1.5s (castingTimeIndex 16).
--
-- Mounts in this codebase are identified by SPELL_AURA_MOUNTED (78) in any
-- effectApplyAuraName slot -- NOT a dedicated SPELL_EFFECT_MOUNT (this
-- codebase's SpellDefines.h has no such effect; mounting is implemented as a
-- generic "apply aura" effect carrying the SPELL_AURA_MOUNTED aura, confirmed
-- in src/game/Spells/SpellAuraDefines.h). 142 spells matched at
-- castingTimeIndex=14 -- every regular/faction/PvP/epic ground mount (Black
-- Ram, Brown Horse, Deathcharger, etc.).
--
-- CORRECTED 2026-08-28: the first pass used castingTimeIndex=5, trusting a
-- comment in MangosSuperUI's PatchController.cs ("1 = instant, 5 = 1.5s,
-- 14 = 2.5s, 15 = 3.0s, 22 = 3.5s") that turned out to be wrong for this
-- server's actual SpellCastTimes.dbc -- confirmed by parsing the real DBC
-- file directly (server/data/5875/dbc/SpellCastTimes.dbc): index 5's Base
-- field is 2000ms, not 1500. Live testing showed ~2s casts, not 1.5s, exactly
-- matching. Index 14 (the original mount value) is genuinely 3000ms, not
-- 2500 as the comment claimed either. The real 1500ms row is index **16**
-- (Base=1500, PerLevel=0, Minimum=1500) -- verified directly against the
-- DBC, not another comment. That PatchController.cs comment is apparently
-- only reliable for index 1 (0ms, instant) and 22 (3500ms) -- the two values
-- it happened to get right; don't trust it for any other index without
-- checking the real DBC first.
UPDATE `spell_template`
SET `castingTimeIndex` = 16
WHERE (`effectApplyAuraName1` = 78 OR `effectApplyAuraName2` = 78 OR `effectApplyAuraName3` = 78)
  AND `castingTimeIndex` IN (5, 14);

-- 16 mount spells were already castingTimeIndex=1 (instant) and are
-- deliberately left untouched: these are NPC-cast/item-triggered/one-off
-- summons (Reindeer, Summon Black Qiraji Battle Tank, Summon Mouth Tentacle,
-- Summon Ivory/Brown/Gray/Pink/Purple/Turquoise Tallstrider, one "(TEST"
-- suffixed entry), not standard player cast-bar mounts -- forcing a cast time
-- onto these would be a behavior change, not a QoL tweak.

-- NOT FIXED HERE, flagged rather than silently patched: the client-visible
-- tooltip still shows the original "3 sec" cast time and will keep doing so
-- regardless of any spell_template edit. These are stock Blizzard-shipped
-- mount spells, not custom_spell_meta-tracked Spell Creator content -- the
-- client's own Spell.dbc (shipped in the base game install, not patch-3.MPQ)
-- has zero knowledge of any server-side spell_template edit for a spell it
-- didn't clone through that pipeline. The mechanism that *would* fix this
-- (adding all 142 spells to custom_spell_meta so RebuildClientPatch clones
-- and overrides their client-side CastingTimeIndex, the same "R1 gameplay
-- override" path already used for the custom Retribution spells) is a real,
-- working option -- but it's patching 142 stock spells through a pipeline
-- built and tested for a handful of custom entries, a materially bigger
-- and riskier undertaking than anything done so far. Deliberately not done
-- without an explicit decision to do so -- the actual enforced cast time
-- (server-side, this file) is correct; only the tooltip text is stale.
