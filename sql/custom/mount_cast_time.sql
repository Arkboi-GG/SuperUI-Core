-- Reduces every player mount spell's cast time to 1.5s (castingTimeIndex 5).
--
-- Mounts in this codebase are identified by SPELL_AURA_MOUNTED (78) in any
-- effectApplyAuraName slot -- NOT a dedicated SPELL_EFFECT_MOUNT (this
-- codebase's SpellDefines.h has no such effect; mounting is implemented as a
-- generic "apply aura" effect carrying the SPELL_AURA_MOUNTED aura, confirmed
-- in src/game/Spells/SpellAuraDefines.h). 142 spells matched at
-- castingTimeIndex=14 (2.5s) -- every regular/faction/PvP/epic ground mount
-- (Black Ram, Brown Horse, Deathcharger, etc.) -- all moved to 5 (1.5s, per
-- the GeneratePatchRequest field comment used elsewhere this session: "1 =
-- instant, 5 = 1.5s, 14 = 2.5s, 15 = 3.0s, 22 = 3.5s").
--
-- 16 mount spells were already castingTimeIndex=1 (instant) and are
-- deliberately left untouched: these are NPC-cast/item-triggered/one-off
-- summons (Reindeer, Summon Black Qiraji Battle Tank, Summon Mouth Tentacle,
-- Summon Ivory/Brown/Gray/Pink/Purple/Turquoise Tallstrider, one "(TEST"
-- suffixed entry), not standard player cast-bar mounts -- forcing a cast time
-- onto these would be a behavior change, not a QoL tweak.
UPDATE `spell_template`
SET `castingTimeIndex` = 5
WHERE (`effectApplyAuraName1` = 78 OR `effectApplyAuraName2` = 78 OR `effectApplyAuraName3` = 78)
  AND `castingTimeIndex` = 14;
