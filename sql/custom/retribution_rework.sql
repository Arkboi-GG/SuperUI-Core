-- Retribution Paladin rework -- data/content half. No talents touched anywhere.
-- Companion C++ changes live in SpellEffects.cpp / SpellAuras.cpp / Unit.cpp / Spell.cpp
-- (see the matching commit). Real spell IDs traced via spell_template's
-- effectTriggerSpell1 indirection layer, not assumed from memory.

-- Exorcism (real spell 10314) and Holy Wrath (real spell 10318):
-- drop the Undead/Demon-only restriction so they work on any target.
UPDATE `spell_template` SET `targetCreatureType` = 0 WHERE `entry` IN (10314, 10318);

-- Hammer of Wrath (real spell 24239): give it an instant cast (was castingTimeIndex 4).
UPDATE `spell_template` SET `castingTimeIndex` = 1 WHERE `entry` = 24239;

-- Consecration (real spell 20924): 8s -> 5s cooldown. IMPORTANT: this spell has a
-- nonzero `category` (932), and SpellCaster.cpp's cooldown code applies BOTH
-- `recoveryTime` and `categoryRecoveryTime` -- whichever is longer wins. Editing only
-- recoveryTime (an earlier mistake here) leaves the old categoryRecoveryTime=8000 in
-- effect and the cooldown doesn't actually change. Both must be set together.
UPDATE `spell_template` SET `recoveryTime` = 5000, `categoryRecoveryTime` = 5000 WHERE `entry` = 20924;

-- Divine Strike template staging (2497-2501): kept for traceability -- these are the
-- deprecated "zzOLDDivine Strike" IDs (originally a no-damage stun) rewritten as an
-- instant AOE weapon strike, Holy Strike's sibling. Same damage shape (effect 58,
-- weapon-required) as Holy Strike, Whirlwind's proven AOE targeting (implicit target
-- 22 / radius 14, verified against live Whirlwind spell 15578), roughly 65% of Holy
-- Strike's per-target damage since it hits everyone in melee range. `category` is
-- explicitly cleared -- the original stun data shared category 65, and leaving that in
-- place while depending on recoveryTime for the new cooldown is exactly the same
-- pitfall Consecration hit above. Not directly taught to anyone anymore (see the
-- 40008-40012 clones below, which are what's actually wired to trainers) but this is
-- what those clones were sourced from, kept in sync for reproducibility.
UPDATE `spell_template` SET
    `effect1` = 58, `effectApplyAuraName1` = 0, `effectImplicitTargetA1` = 22, `effectRadiusIndex1` = 14,
    `rangeIndex` = 1, `castingTimeIndex` = 1, `recoveryTime` = 12000, `category` = 0, `categoryRecoveryTime` = 0,
    `equippedItemClass` = 2, `equippedItemSubClassMask` = 173555, `powerType` = 0,
    `spellLevel` = 8, `baseLevel` = 8, `effectBasePoints1` = 12, `effectDieSides1` = 4, `manaCost` = 45
WHERE `entry` = 2497;
UPDATE `spell_template` SET
    `effect1` = 58, `effectApplyAuraName1` = 0, `effectImplicitTargetA1` = 22, `effectRadiusIndex1` = 14,
    `rangeIndex` = 1, `castingTimeIndex` = 1, `recoveryTime` = 12000, `category` = 0, `categoryRecoveryTime` = 0,
    `equippedItemClass` = 2, `equippedItemSubClassMask` = 173555, `powerType` = 0,
    `spellLevel` = 22, `baseLevel` = 22, `effectBasePoints1` = 38, `effectDieSides1` = 8, `manaCost` = 90
WHERE `entry` = 2498;
UPDATE `spell_template` SET
    `effect1` = 58, `effectApplyAuraName1` = 0, `effectImplicitTargetA1` = 22, `effectRadiusIndex1` = 14,
    `rangeIndex` = 1, `castingTimeIndex` = 1, `recoveryTime` = 12000, `category` = 0, `categoryRecoveryTime` = 0,
    `equippedItemClass` = 2, `equippedItemSubClassMask` = 173555, `powerType` = 0,
    `spellLevel` = 36, `baseLevel` = 36, `effectBasePoints1` = 78, `effectDieSides1` = 12, `manaCost` = 140
WHERE `entry` = 2499;
UPDATE `spell_template` SET
    `effect1` = 58, `effectApplyAuraName1` = 0, `effectImplicitTargetA1` = 22, `effectRadiusIndex1` = 14,
    `rangeIndex` = 1, `castingTimeIndex` = 1, `recoveryTime` = 12000, `category` = 0, `categoryRecoveryTime` = 0,
    `equippedItemClass` = 2, `equippedItemSubClassMask` = 173555, `powerType` = 0,
    `spellLevel` = 50, `baseLevel` = 50, `effectBasePoints1` = 130, `effectDieSides1` = 18, `manaCost` = 190
WHERE `entry` = 2500;
UPDATE `spell_template` SET
    `effect1` = 58, `effectApplyAuraName1` = 0, `effectImplicitTargetA1` = 22, `effectRadiusIndex1` = 14,
    `rangeIndex` = 1, `castingTimeIndex` = 1, `recoveryTime` = 12000, `category` = 0, `categoryRecoveryTime` = 0,
    `equippedItemClass` = 2, `equippedItemSubClassMask` = 173555, `powerType` = 0,
    `spellLevel` = 60, `baseLevel` = 60, `effectBasePoints1` = 160, `effectDieSides1` = 20, `manaCost` = 230
WHERE `entry` = 2501;

-- Player-visible spells: cloned via MSUI's Spell Creator (POST /Patch/Generate --
-- see the WIKI's [[Spell Creator Workflow]] note for the full method), which is the
-- only supported way to get a correct name/icon/tooltip into the client without hand
-- patching Spell.dbc. Cloned FROM the reworked zzOLD/staging entries above, so they
-- inherit all the mechanics set there. Holy Strike keeps the real 8 authentic ranks
-- (level 1-56); Divine Strike uses the new 5-rank ladder (level 8-60); Zealotry
-- (40013, cloned from Clearcasting/16246) is the Retribution crit-proc buff cast by
-- Unit.cpp's DealMeleeDamage and consumed by Spell.cpp's Exorcism check -- it is
-- NOT taught to anyone, just referenced by spell ID from C++.
REPLACE INTO `spell_template` (`entry`, `build`, `school`, `category`, `castUI`, `dispel`, `mechanic`, `attributes`, `attributesEx`, `attributesEx2`, `attributesEx3`, `attributesEx4`, `stances`, `stancesNot`, `targets`, `targetCreatureType`, `requiresSpellFocus`, `casterAuraState`, `targetAuraState`, `castingTimeIndex`, `recoveryTime`, `categoryRecoveryTime`, `interruptFlags`, `auraInterruptFlags`, `channelInterruptFlags`, `procFlags`, `procChance`, `procCharges`, `maxLevel`, `baseLevel`, `spellLevel`, `durationIndex`, `powerType`, `manaCost`, `manCostPerLevel`, `manaPerSecond`, `manaPerSecondPerLevel`, `rangeIndex`, `speed`, `modelNextSpell`, `stackAmount`, `totem1`, `totem2`, `reagent1`, `reagent2`, `reagent3`, `reagent4`, `reagent5`, `reagent6`, `reagent7`, `reagent8`, `reagentCount1`, `reagentCount2`, `reagentCount3`, `reagentCount4`, `reagentCount5`, `reagentCount6`, `reagentCount7`, `reagentCount8`, `equippedItemClass`, `equippedItemSubClassMask`, `equippedItemInventoryTypeMask`, `effect1`, `effect2`, `effect3`, `effectDieSides1`, `effectDieSides2`, `effectDieSides3`, `effectBaseDice1`, `effectBaseDice2`, `effectBaseDice3`, `effectDicePerLevel1`, `effectDicePerLevel2`, `effectDicePerLevel3`, `effectRealPointsPerLevel1`, `effectRealPointsPerLevel2`, `effectRealPointsPerLevel3`, `effectBasePoints1`, `effectBasePoints2`, `effectBasePoints3`, `effectBonusCoefficient1`, `effectBonusCoefficient2`, `effectBonusCoefficient3`, `effectMechanic1`, `effectMechanic2`, `effectMechanic3`, `effectImplicitTargetA1`, `effectImplicitTargetA2`, `effectImplicitTargetA3`, `effectImplicitTargetB1`, `effectImplicitTargetB2`, `effectImplicitTargetB3`, `effectRadiusIndex1`, `effectRadiusIndex2`, `effectRadiusIndex3`, `effectApplyAuraName1`, `effectApplyAuraName2`, `effectApplyAuraName3`, `effectAmplitude1`, `effectAmplitude2`, `effectAmplitude3`, `effectMultipleValue1`, `effectMultipleValue2`, `effectMultipleValue3`, `effectChainTarget1`, `effectChainTarget2`, `effectChainTarget3`, `effectItemType1`, `effectItemType2`, `effectItemType3`, `effectMiscValue1`, `effectMiscValue2`, `effectMiscValue3`, `effectTriggerSpell1`, `effectTriggerSpell2`, `effectTriggerSpell3`, `effectPointsPerComboPoint1`, `effectPointsPerComboPoint2`, `effectPointsPerComboPoint3`, `spellVisual1`, `spellVisual2`, `spellIconId`, `activeIconId`, `spellPriority`, `name`, `nameFlags`, `nameSubtext`, `nameSubtextFlags`, `description`, `descriptionFlags`, `auraDescription`, `auraDescriptionFlags`, `manaCostPercentage`, `startRecoveryCategory`, `startRecoveryTime`, `minTargetLevel`, `maxTargetLevel`, `spellFamilyName`, `spellFamilyFlags`, `maxAffectedTargets`, `dmgClass`, `preventionType`, `stanceBarOrder`, `dmgMultiplier1`, `dmgMultiplier2`, `dmgMultiplier3`, `minFactionId`, `minReputation`, `requiredAuraVision`, `customFlags`, `script_name`) VALUES (40000,4222,1,40,0,0,0,328708,0,0,0,0,0,0,0,0,0,0,0,1,0,10000,0,0,0,0,101,0,0,1,99,0,0,20,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,3,0,0,1,0,0,0,0,0,0,0,0,9,0,0,-1,-1,-1,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10000,0,52,0,0,'Holy Strike',983070,'Rank 1',7274526,'Consecrates your weapon, inflicting $s1 additional damage on your next attack. All damage caused is considered holy damage.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40001,4222,1,40,0,0,0,328708,0,0,0,0,0,0,0,0,0,0,0,1,0,10000,0,0,0,0,101,0,0,8,99,0,0,40,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,5,0,0,1,0,0,0,0,0,0,0,0,19,0,0,-1,-1,-1,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10001,0,52,0,0,'Holy Strike',983070,'Rank 2',7274526,'Consecrates your weapon, inflicting $s1 additional damage on your next attack. All damage caused is considered holy damage.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40002,4222,1,40,0,0,0,328708,0,0,0,0,0,0,0,0,0,0,0,1,0,10000,0,0,0,0,101,0,0,16,99,0,0,60,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,7,0,0,1,0,0,0,0,0,0,0,0,35,0,0,-1,-1,-1,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10002,0,52,0,0,'Holy Strike',983070,'Rank 3',7274526,'Consecrates your weapon, inflicting $s1 additional damage on your next attack. All damage caused is considered holy damage.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40003,4222,1,40,0,0,0,328708,0,0,0,0,0,0,0,0,0,0,0,1,0,10000,0,0,0,0,101,0,0,24,99,0,0,85,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,9,0,0,1,0,0,0,0,0,0,0,0,57,0,0,-1,-1,-1,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10003,0,52,0,0,'Holy Strike',983070,'Rank 4',7274526,'Consecrates your weapon, inflicting $s1 additional damage on your next attack. All damage caused is considered holy damage.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40004,4222,1,40,0,0,0,328708,0,0,0,0,0,0,0,0,0,0,0,1,0,10000,0,0,0,0,101,0,0,32,99,0,0,120,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,23,0,0,1,0,0,0,0,0,0,0,0,89,0,0,-1,-1,-1,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10004,0,52,0,0,'Holy Strike',983070,'Rank 5',7274526,'Consecrates your weapon, inflicting $s1 additional damage on your next attack. All damage caused is considered holy damage.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40005,4222,1,40,0,0,0,328708,0,0,0,0,0,0,0,0,0,0,0,1,0,10000,0,0,0,0,101,0,0,40,99,0,0,150,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,17,0,0,1,0,0,0,0,0,0,0,0,120,0,0,-1,-1,-1,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10005,0,52,0,0,'Holy Strike',983070,'Rank 6',7274526,'Consecrates your weapon, inflicting $s1 additional damage on your next attack. All damage caused is considered holy damage.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40006,4222,1,40,0,0,0,328708,0,0,0,0,0,0,0,0,0,0,0,1,0,10000,0,0,0,0,101,0,0,48,99,0,0,180,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,21,0,0,1,0,0,0,0,0,0,0,0,156,0,0,-1,-1,-1,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10006,0,52,0,0,'Holy Strike',983070,'Rank 7',983070,'Consecrates your weapon, inflicting $s1 additional damage on your next attack. All damage caused is considered holy damage.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40007,4222,1,40,0,0,0,328708,0,0,0,0,0,0,0,0,0,0,0,1,0,10000,0,0,0,0,101,0,0,56,99,0,0,215,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,25,0,0,1,0,0,0,0,0,0,0,0,200,0,0,-1,-1,-1,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10007,0,52,0,0,'Holy Strike',983070,'Rank 8',983070,'Consecrates your weapon, inflicting $s1 additional damage on your next attack. All damage caused is considered holy damage.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40008,4222,1,0,0,0,0,263184,0,0,0,0,0,0,0,0,0,0,0,1,12000,0,0,0,0,0,101,0,0,8,8,35,0,45,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,4,0,0,1,0,0,0,0,0,0,0,0,12,0,0,-1,-1,-1,0,0,0,22,0,0,0,0,0,14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10008,0,52,0,50,'Divine Strike',7274526,'Rank 1',7274526,'The paladin\'s next attack will stun the enemy for $d.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40009,4222,1,0,0,0,0,263184,0,0,0,0,0,0,0,0,0,0,0,1,12000,0,0,0,0,0,101,0,0,22,22,32,0,90,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,8,0,0,1,0,0,0,0,0,0,0,0,38,0,0,-1,-1,-1,0,0,0,22,0,0,0,0,0,14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10009,0,52,0,50,'Divine Strike',7274526,'Rank 2',7274526,'The paladin\'s next attack will stun the enemy for $d.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40010,4222,1,0,0,0,0,263184,0,0,0,0,0,0,0,0,0,0,0,1,12000,0,0,0,0,0,101,0,0,36,36,31,0,140,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,12,0,0,1,0,0,0,0,0,0,0,0,78,0,0,-1,-1,-1,0,0,0,22,0,0,0,0,0,14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10010,0,52,0,50,'Divine Strike',7274526,'Rank 3',7274526,'The paladin\'s next attack will stun the enemy for $d.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40011,4222,1,0,0,0,0,263184,0,0,0,0,0,0,0,0,0,0,0,1,12000,0,0,0,0,0,101,0,0,50,50,31,0,190,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,18,0,0,1,0,0,0,0,0,0,0,0,130,0,0,-1,-1,-1,0,0,0,22,0,0,0,0,0,14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10011,0,52,0,50,'Divine Strike',7274526,'Rank 4',7274526,'The paladin\'s next attack will stun the enemy for $d.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40012,4222,1,0,0,0,0,263184,0,0,0,0,0,0,0,0,0,0,0,1,12000,0,0,0,0,0,101,0,0,60,60,31,0,230,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,173555,0,58,0,0,20,0,0,1,0,0,0,0,0,0,0,0,160,0,0,-1,-1,-1,0,0,0,22,0,0,0,0,0,14,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,10012,0,52,0,50,'Divine Strike',7274526,'Rank 5',7274526,'The paladin\'s next attack will stun the enemy for $d.',7274526,'',983052,0,0,0,0,0,0,0,0,2,2,-1,1,1,1,0,0,0,0,''),
(40013,5302,6,0,0,1,0,327680,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,87376,100,1,0,10,10,8,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-1,-1,0,6,0,0,1,0,0,1,0,0,0,0,0,0,0,0,-101,0,0,0,-1,-1,0,0,0,1,0,0,0,0,0,0,0,0,108,0,0,0,0,0,0,0,0,0,0,0,2416967683,0,0,14,0,0,0,0,0,0,0,0,10013,0,64,0,0,'Zealotry',2031678,'Rank 1',2031676,'',2031676,'Your next elemental damage spell has its mana cost reduced by $s1%.',2031678,0,0,0,0,0,11,0,0,1,1,-1,1,1,1,0,0,0,0,'');

-- Teach both strikes (the cloned, properly-named entries above) through the Paladin
-- trainer templates (28, 29 -- same ones serving every real baseline Paladin spell).
-- Also removes the zzOLD-named originals from the trainer if an earlier run of this
-- file (before the clone step existed) taught them directly.
DELETE FROM `npc_trainer_template` WHERE `entry` IN (28, 29) AND `spell` IN
    (679, 678, 1866, 680, 2495, 5569, 10332, 10333, 2497, 2498, 2499, 2500, 2501);
DELETE FROM `npc_trainer_template` WHERE `entry` IN (28, 29) AND `spell` BETWEEN 40000 AND 40012;
INSERT INTO `npc_trainer_template` (`entry`, `spell`, `spellcost`, `reqskill`, `reqskillvalue`, `reqlevel`) VALUES
(28, 40000, 0, 0, 0, 1),  (28, 40001, 0, 0, 0, 8),  (28, 40002, 0, 0, 0, 16), (28, 40003, 0, 0, 0, 24),
(28, 40004, 0, 0, 0, 32), (28, 40005, 0, 0, 0, 40), (28, 40006, 0, 0, 0, 48), (28, 40007, 0, 0, 0, 56),
(28, 40008, 0, 0, 0, 8),  (28, 40009, 0, 0, 0, 22), (28, 40010, 0, 0, 0, 36), (28, 40011, 0, 0, 0, 50), (28, 40012, 0, 0, 0, 60),
(29, 40000, 0, 0, 0, 1),  (29, 40001, 0, 0, 0, 8),  (29, 40002, 0, 0, 0, 16), (29, 40003, 0, 0, 0, 24),
(29, 40004, 0, 0, 0, 32), (29, 40005, 0, 0, 0, 40), (29, 40006, 0, 0, 0, 48), (29, 40007, 0, 0, 0, 56),
(29, 40008, 0, 0, 0, 8),  (29, 40009, 0, 0, 0, 22), (29, 40010, 0, 0, 0, 36), (29, 40011, 0, 0, 0, 50), (29, 40012, 0, 0, 0, 60);

-- Spellbook categorization: being trainer-taught is NOT enough to land in the
-- Paladin tab -- that's driven separately by `skill_line_ability`. The Spell
-- Creator's own "auto-copy from source" step (PatchController.cs) only copies an
-- SLA row if the SOURCE spell already had one, and these zzOLD-derived sources
-- never did (they predate/were excluded from this categorization system). Without
-- this block the clones would sit in the generic/uncategorized tab. skill_id 184 +
-- class_mask 2 is the real, verified pattern (checked against the live Blessing of
-- Might family, which uses exactly this) -- not chained via superseded_by_spell,
-- matching how real multi-rank Paladin spells are actually wired.
DELETE FROM `skill_line_ability` WHERE `spell_id` BETWEEN 40000 AND 40012 AND `build` = 5875;
INSERT INTO `skill_line_ability` (`id`, `build`, `skill_id`, `spell_id`, `race_mask`, `class_mask`, `req_skill_value`, `superseded_by_spell`, `learn_on_get_skill`, `max_value`, `min_value`, `req_train_points`)
SELECT (SELECT COALESCE(MAX(id), 0) FROM skill_line_ability) + n, 5875, 184, spell, 0, 2, 1, 0, 0, 0, 0, 0
FROM (
    SELECT 1 AS n, 40000 AS spell UNION ALL SELECT 2, 40001 UNION ALL SELECT 3, 40002 UNION ALL SELECT 4, 40003
    UNION ALL SELECT 5, 40004 UNION ALL SELECT 6, 40005 UNION ALL SELECT 7, 40006 UNION ALL SELECT 8, 40007
    UNION ALL SELECT 9, 40008 UNION ALL SELECT 10, 40009 UNION ALL SELECT 11, 40010 UNION ALL SELECT 12, 40011
    UNION ALL SELECT 13, 40012
) ranks;
