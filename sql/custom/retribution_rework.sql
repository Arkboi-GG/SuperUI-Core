-- Retribution Paladin rework -- data/content half. No talents touched anywhere.
-- Companion C++ changes live in SpellEffects.cpp / SpellAuras.cpp / Unit.cpp / Spell.cpp
-- (see the matching commit). Real spell IDs traced via spell_template's
-- effectTriggerSpell1 indirection layer, not assumed from memory.

-- Exorcism (real spell 10314) and Holy Wrath (real spell 10318):
-- drop the Undead/Demon-only restriction so they work on any target.
UPDATE `spell_template` SET `targetCreatureType` = 0 WHERE `entry` IN (10314, 10318);

-- Consecration (real spell 20924): 8s -> 5s cooldown, already instant cast.
UPDATE `spell_template` SET `recoveryTime` = 5000 WHERE `entry` = 20924;

-- Hammer of Wrath (real spell 24239): give it an instant cast (was castingTimeIndex 4).
UPDATE `spell_template` SET `castingTimeIndex` = 1 WHERE `entry` = 24239;

-- Divine Strike: rebuilt from the deprecated "zzOLDDivine Strike" IDs (2497-2501),
-- which were originally a stun with no damage. Rewritten as an instant AOE weapon
-- strike, Holy Strike's sibling -- same damage shape (effect 58, weapon-required),
-- Whirlwind's proven AOE targeting (implicit target 22 / radius 14, verified against
-- the live Whirlwind spell 15578), roughly 65% of Holy Strike's per-target damage
-- since it hits everyone in melee range. New 5-rank ladder up to level 60 (the
-- original 5 ranks only went to level 37). Tune damage/mana/cooldown freely later.
UPDATE `spell_template` SET
    `effect1` = 58, `effectApplyAuraName1` = 0, `effectImplicitTargetA1` = 22, `effectRadiusIndex1` = 14,
    `rangeIndex` = 1, `castingTimeIndex` = 1, `recoveryTime` = 12000, `categoryRecoveryTime` = 0,
    `equippedItemClass` = 2, `equippedItemSubClassMask` = 173555, `powerType` = 0,
    `spellLevel` = 8, `baseLevel` = 8, `effectBasePoints1` = 12, `effectDieSides1` = 4, `manaCost` = 45
WHERE `entry` = 2497;
UPDATE `spell_template` SET
    `effect1` = 58, `effectApplyAuraName1` = 0, `effectImplicitTargetA1` = 22, `effectRadiusIndex1` = 14,
    `rangeIndex` = 1, `castingTimeIndex` = 1, `recoveryTime` = 12000, `categoryRecoveryTime` = 0,
    `equippedItemClass` = 2, `equippedItemSubClassMask` = 173555, `powerType` = 0,
    `spellLevel` = 22, `baseLevel` = 22, `effectBasePoints1` = 38, `effectDieSides1` = 8, `manaCost` = 90
WHERE `entry` = 2498;
UPDATE `spell_template` SET
    `effect1` = 58, `effectApplyAuraName1` = 0, `effectImplicitTargetA1` = 22, `effectRadiusIndex1` = 14,
    `rangeIndex` = 1, `castingTimeIndex` = 1, `recoveryTime` = 12000, `categoryRecoveryTime` = 0,
    `equippedItemClass` = 2, `equippedItemSubClassMask` = 173555, `powerType` = 0,
    `spellLevel` = 36, `baseLevel` = 36, `effectBasePoints1` = 78, `effectDieSides1` = 12, `manaCost` = 140
WHERE `entry` = 2499;
UPDATE `spell_template` SET
    `effect1` = 58, `effectApplyAuraName1` = 0, `effectImplicitTargetA1` = 22, `effectRadiusIndex1` = 14,
    `rangeIndex` = 1, `castingTimeIndex` = 1, `recoveryTime` = 12000, `categoryRecoveryTime` = 0,
    `equippedItemClass` = 2, `equippedItemSubClassMask` = 173555, `powerType` = 0,
    `spellLevel` = 50, `baseLevel` = 50, `effectBasePoints1` = 130, `effectDieSides1` = 18, `manaCost` = 190
WHERE `entry` = 2500;
UPDATE `spell_template` SET
    `effect1` = 58, `effectApplyAuraName1` = 0, `effectImplicitTargetA1` = 22, `effectRadiusIndex1` = 14,
    `rangeIndex` = 1, `castingTimeIndex` = 1, `recoveryTime` = 12000, `categoryRecoveryTime` = 0,
    `equippedItemClass` = 2, `equippedItemSubClassMask` = 173555, `powerType` = 0,
    `spellLevel` = 60, `baseLevel` = 60, `effectBasePoints1` = 160, `effectDieSides1` = 20, `manaCost` = 230
WHERE `entry` = 2501;

-- Teach both strikes through the Paladin trainer templates (28, 29 -- same ones
-- serving every real baseline Paladin spell). Holy Strike keeps its 8 authentic
-- ranks/levels; Divine Strike uses the new 5-rank ladder above.
DELETE FROM `npc_trainer_template` WHERE `entry` IN (28, 29) AND `spell` IN
    (679, 678, 1866, 680, 2495, 5569, 10332, 10333, 2497, 2498, 2499, 2500, 2501);
INSERT INTO `npc_trainer_template` (`entry`, `spell`, `spellcost`, `reqskill`, `reqskillvalue`, `reqlevel`) VALUES
(28, 679,   0, 0, 0, 1),  (28, 678,   0, 0, 0, 8),  (28, 1866,  0, 0, 0, 16), (28, 680,   0, 0, 0, 24),
(28, 2495,  0, 0, 0, 32), (28, 5569,  0, 0, 0, 40), (28, 10332, 0, 0, 0, 48), (28, 10333, 0, 0, 0, 56),
(28, 2497,  0, 0, 0, 8),  (28, 2498,  0, 0, 0, 22), (28, 2499,  0, 0, 0, 36), (28, 2500,  0, 0, 0, 50), (28, 2501, 0, 0, 0, 60),
(29, 679,   0, 0, 0, 1),  (29, 678,   0, 0, 0, 8),  (29, 1866,  0, 0, 0, 16), (29, 680,   0, 0, 0, 24),
(29, 2495,  0, 0, 0, 32), (29, 5569,  0, 0, 0, 40), (29, 10332, 0, 0, 0, 48), (29, 10333, 0, 0, 0, 56),
(29, 2497,  0, 0, 0, 8),  (29, 2498,  0, 0, 0, 22), (29, 2499,  0, 0, 0, 36), (29, 2500,  0, 0, 0, 50), (29, 2501, 0, 0, 0, 60);
