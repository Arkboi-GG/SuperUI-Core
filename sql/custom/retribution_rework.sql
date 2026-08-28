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
-- trainer templates (28, 29 -- confirmed via creature_template.trainer_id the ONLY
-- two template IDs used by any trainer_class=2 NPC in the game, e.g. Lord Grayson
-- Shadowbreaker/entry 928 -> 29 -- so these two cover every real Paladin trainer,
-- every race/city, no others to fix).
-- Also removes the zzOLD-named originals from the trainer if an earlier run of this
-- file (before the clone step existed) taught them directly.
DELETE FROM `npc_trainer_template` WHERE `entry` IN (28, 29) AND `spell` IN
    (679, 678, 1866, 680, 2495, 5569, 10332, 10333, 2497, 2498, 2499, 2500, 2501);

-- Found live at Lord Grayson Shadowbreaker (2026-08-26): inserting 40000-40012
-- directly into npc_trainer_template (an earlier version of this block) made them
-- silently invisible at every Paladin trainer, no error client-side. Root cause is
-- server-side, at load, not a reqlevel/reqskill/class issue: ObjectMgr::LoadTrainers
-- (ObjectMgr.cpp) requires every spell referenced by npc_trainer_template.spell to
-- have effect1 == SPELL_EFFECT_LEARN_SPELL (36) -- anything else is rejected at boot
-- with "has non-learning spell N, ignore" (confirmed in Server.log) and never even
-- reaches the in-memory trainer spell list, regardless of req* columns. Every real
-- baseline Paladin spell already follows this: npc_trainer_template.spell holds a
-- small "trainer wrapper" spell (e.g. Judgement's wrapper 10321 has
-- effect1=36/effectTriggerSpell1=20271, the real Judgement) -- SendTrainerList
-- (NPCHandler.cpp) reads EffectTriggerSpell[0] off *that* wrapper, not off
-- npc_trainer_template.spell directly, to run the class/race fit check. 40000-40012
-- are real damage spells (effect1=58), not wrappers, so they need one each -- built
-- via the exact same shape MangosSuperUI's own CreateTrainerWrapperAsync uses
-- (SpellCreatorService.cs), in its dedicated wrapper ID range (50000-65000).
REPLACE INTO `spell_template` (`entry`, `build`, `school`, `attributes`, `targets`, `procChance`,
    `equippedItemClass`, `equippedItemSubClassMask`, `effect1`, `effectTriggerSpell1`,
    `spellVisual1`, `spellIconId`, `castingTimeIndex`, `name`, `nameFlags`, `nameSubtext`, `nameSubtextFlags`,
    `description`, `descriptionFlags`, `auraDescription`, `auraDescriptionFlags`,
    `rangeIndex`, `dmgMultiplier1`, `dmgMultiplier2`, `dmgMultiplier3`,
    `effectBonusCoefficient1`, `effectBonusCoefficient2`, `effectBonusCoefficient3`, `stanceBarOrder`, `spellLevel`, `baseLevel`) VALUES
(50000, 5875, 0, 262400, 256, 101, -1, -1, 36, 40000, 107, 52, 1, 'Holy Strike', 983070, 'Rank 1', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0),
(50001, 5875, 0, 262400, 256, 101, -1, -1, 36, 40001, 107, 52, 1, 'Holy Strike', 983070, 'Rank 2', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0),
(50002, 5875, 0, 262400, 256, 101, -1, -1, 36, 40002, 107, 52, 1, 'Holy Strike', 983070, 'Rank 3', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0),
(50003, 5875, 0, 262400, 256, 101, -1, -1, 36, 40003, 107, 52, 1, 'Holy Strike', 983070, 'Rank 4', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0),
(50004, 5875, 0, 262400, 256, 101, -1, -1, 36, 40004, 107, 52, 1, 'Holy Strike', 983070, 'Rank 5', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0),
(50005, 5875, 0, 262400, 256, 101, -1, -1, 36, 40005, 107, 52, 1, 'Holy Strike', 983070, 'Rank 6', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0),
(50006, 5875, 0, 262400, 256, 101, -1, -1, 36, 40006, 107, 52, 1, 'Holy Strike', 983070, 'Rank 7', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0),
(50007, 5875, 0, 262400, 256, 101, -1, -1, 36, 40007, 107, 52, 1, 'Holy Strike', 983070, 'Rank 8', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0),
(50008, 5875, 0, 262400, 256, 101, -1, -1, 36, 40008, 107, 52, 1, 'Divine Strike', 983070, 'Rank 1', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0),
(50009, 5875, 0, 262400, 256, 101, -1, -1, 36, 40009, 107, 52, 1, 'Divine Strike', 983070, 'Rank 2', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0),
(50010, 5875, 0, 262400, 256, 101, -1, -1, 36, 40010, 107, 52, 1, 'Divine Strike', 983070, 'Rank 3', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0),
(50011, 5875, 0, 262400, 256, 101, -1, -1, 36, 40011, 107, 52, 1, 'Divine Strike', 983070, 'Rank 4', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0),
(50012, 5875, 0, 262400, 256, 101, -1, -1, 36, 40012, 107, 52, 1, 'Divine Strike', 983070, 'Rank 5', 983070, '', 983052, '', 983052, 6, 1, 1, 1, 0, -1, -1, -1, 0, 0);

DELETE FROM `npc_trainer_template` WHERE `entry` IN (28, 29) AND `spell` BETWEEN 40000 AND 40012;
DELETE FROM `npc_trainer_template` WHERE `entry` IN (28, 29) AND `spell` BETWEEN 50000 AND 50012;
INSERT INTO `npc_trainer_template` (`entry`, `spell`, `spellcost`, `reqskill`, `reqskillvalue`, `reqlevel`) VALUES
(28, 50000, 0, 0, 0, 1),  (28, 50001, 0, 0, 0, 8),  (28, 50002, 0, 0, 0, 16), (28, 50003, 0, 0, 0, 24),
(28, 50004, 0, 0, 0, 32), (28, 50005, 0, 0, 0, 40), (28, 50006, 0, 0, 0, 48), (28, 50007, 0, 0, 0, 56),
(28, 50008, 0, 0, 0, 8),  (28, 50009, 0, 0, 0, 22), (28, 50010, 0, 0, 0, 36), (28, 50011, 0, 0, 0, 50), (28, 50012, 0, 0, 0, 60),
(29, 50000, 0, 0, 0, 1),  (29, 50001, 0, 0, 0, 8),  (29, 50002, 0, 0, 0, 16), (29, 50003, 0, 0, 0, 24),
(29, 50004, 0, 0, 0, 32), (29, 50005, 0, 0, 0, 40), (29, 50006, 0, 0, 0, 48), (29, 50007, 0, 0, 0, 56),
(29, 50008, 0, 0, 0, 8),  (29, 50009, 0, 0, 0, 22), (29, 50010, 0, 0, 0, 36), (29, 50011, 0, 0, 0, 50), (29, 50012, 0, 0, 0, 60);

-- Zealotry (40013) presentation fix (found from live testing, 2026-08-26): the
-- Spell Creator clone above inherited Clearcasting's real gameplay shape
-- verbatim -- correct for the mechanic (Spell.cpp/Unit.cpp reference 40013
-- directly and only ever CastSpell(this, 40013, true) it server-side), but
-- wrong for presentation. Two problems:
--   1. rangeIndex 6 (40yd) with no SPELL_ATTR_PASSIVE meant a GM `.learn`ing it
--      for testing saw it in the spellbook as a directly-castable, ranged
--      instant spell -- Zealotry is only ever applied by the DealMeleeDamage
--      proc, never player-cast. rangeIndex -> 1 (self) and attributes gains
--      SPELL_ATTR_PASSIVE (327680 -> 327744, +0x40) -- the exact pattern
--      MangosSuperUI's own SuiHero.cpp requires for its hidden hero buffs.
--      HandleCastSpellOpcode rejects any client-issued cast of a passive
--      spell outright, regardless of what a stale client DBC still shows.
--   2. auraDescription still literally read Clearcasting's real tooltip
--      ("Your next elemental damage spell has its mana cost reduced by
--      $s1%.") -- the original clone only overrode name/icon, never
--      description/tooltip. Fixed here on spell_template for documentation;
--      spell_template.description/auraDescription are NOT what the client
--      actually reads (see [[Spell Pages]]'s corrected-assumption note) --
--      the real fix is `custom_spell_meta.tooltip` below, delivered via
--      MSUI's `POST /Patch/RebuildClientPatch`. nameSubtext cleared too --
--      "Rank 1" is meaningless on a single-rank passive.
UPDATE `spell_template` SET
    `attributes` = 327744, `rangeIndex` = 1, `nameSubtext` = '',
    `auraDescription` = 'Holy Shock and, if the target is below 20% health, Hammer of Wrath have had their cooldowns reset. Your next Exorcism is instant and costs no mana.'
WHERE `entry` = 40013;
UPDATE `custom_spell_meta` SET
    `tooltip` = 'Holy Shock and, if the target is below 20% health, Hammer of Wrath have had their cooldowns reset. Your next Exorcism is instant and costs no mana.',
    `name_subtext` = ''
WHERE `entry` = 40013;

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

-- Divine Strike doesn't cast at all (found live, 2026-08-27): `attributes` on both
-- the zzOLD staging entries (2497-2501) and the real trainer-taught clones
-- (40008-40012) still carried SPELL_ATTR_ON_NEXT_SWING (0x400, part of 263184) --
-- inherited unchanged from Holy Strike's attribute set when the stun data was
-- reworked into an AOE strike, since only the effect/targeting columns were
-- touched at the time, not attributes. SpellEntry::IsNextMeleeSwingSpell()
-- (checked via GetCurrentContainer() in Spell.cpp) treats ANY spell with this bit
-- as a queued "next melee swing" special attack (like Heroic Strike) instead of an
-- instant spell -- it's filed into CURRENT_MELEE_SPELL and only fires on the
-- caster's next successful autoattack, never CURRENT_GENERIC_SPELL, so a cast that
-- isn't followed by a landed melee swing looks like nothing happened at all.
-- Whirlwind (15578, the spell this AOE targeting was copied from) has attributes
-- 262160 -- exactly 1024 (0x400) less -- confirming the extra bit was never meant
-- to be there. Holy Strike is unaffected: it's SUPPOSED to be a next-swing ability
-- (real tooltip: "additional damage on your next attack"), so its 328708 is correct
-- and untouched.
UPDATE `spell_template` SET `attributes` = 262160
    WHERE `entry` IN (2497, 2498, 2499, 2500, 2501, 40008, 40009, 40010, 40011, 40012);

-- Divine Strike gives "Invalid target" on cast (found live, 2026-08-27, after
-- the attributes fix above got it actually attempting to cast): the AOE
-- rework only ever set `effectImplicitTargetA1`=22 (TARGET_LOCATION_CASTER_SRC
-- -- a location, not a unit) and `effectRadiusIndex1`=14, copied from
-- Whirlwind (15578) -- but never copied Whirlwind's matching
-- `effectImplicitTargetB1`=15 (TARGET_ENUM_UNITS_ENEMY_AOE_AT_SRC_LOC, the
-- actual "select enemies in radius of that location" criteria the A-target
-- pairs with). Left at 0, Divine Strike had a location target with no unit-
-- selection attached to it -- nothing for the server to ever resolve as a
-- valid target, hence "Invalid target" on every cast attempt. effect2/3 and
-- their own implicit targets were NOT the cause -- confirmed live, both are
-- already 0/0 on every rank and on Whirlwind itself (real damage is entirely
-- effect1; the zzOLD stun mechanic was apparently also entirely effect1, not
-- spread across effect2/3). Fixed by copying Whirlwind's `effectImplicitTargetB1`
-- exactly, on both the zzOLD staging entries and the real clones.
UPDATE `spell_template` SET `effectImplicitTargetB1` = 15
    WHERE `entry` IN (2497, 2498, 2499, 2500, 2501, 40008, 40009, 40010, 40011, 40012);

-- Divine Strike's tooltip + both strikes' icon (found live, 2026-08-27): the
-- Generate() clone never set an icon override, so `GetSchoolIconId(school)` picked
-- a generic Holy-school fallback (52) for both -- but 679/678/.../10333 (Holy
-- Strike's real zzOLD sources) and 2497-2501 (Divine Strike's) are genuine
-- pre-launch-cut Blizzard content and already shipped with distinct, purpose-made
-- icons (25 and 42 respectively, confirmed by reading the pristine DBC rows
-- directly) -- reusing those via `icon_source='source'` beats a generic fallback.
-- Divine Strike's own description was also never updated after the stun-to-AOE
-- rework: it still read the original zzOLD stun blurb ("next attack will stun the
-- enemy for $d"). Both fixes go through `custom_spell_meta` + a client-patch
-- rebuild (`POST /Patch/RebuildClientPatch`), same channel as the Zealotry fix --
-- spell_template's own description/icon columns are not what the client reads.
UPDATE `custom_spell_meta` SET `icon_source` = 'source' WHERE `entry` BETWEEN 40000 AND 40012;
UPDATE `custom_spell_meta` SET `description` = 'An instant strike that hits all enemies within melee range, dealing weapon damage plus $s1 additional damage. All damage caused is considered holy damage.'
    WHERE `entry` BETWEEN 40008 AND 40012;

-- Trainer wrapper spells (50000-50012, see the npc_trainer_template fix above)
-- were only ever inserted into `spell_template` via raw SQL, never through
-- `custom_spell_meta` -- meaning `RebuildUnifiedPatchFromConfigsAsync` (which
-- drives every client patch rebuild) had no idea they existed, so the client's
-- own Spell.dbc never got a row for them at all. A trainer offer the client can't
-- look up in its own Spell.dbc renders as nothing, which is why they stayed
-- invisible at Lord Grayson Shadowbreaker even after the wrapper fix + a full
-- service restart. Sourced from the same pristine zzOLD originals as the real
-- spells (not from 40000-40012) so the trainer-window icon/description are
-- correct and never depend on cache staleness from the custom entries.
INSERT INTO `custom_spell_meta` (`entry`, `source_entry`, `spell_name`, `name_subtext`, `icon_source`) VALUES
(50000, 679,   'Holy Strike', 'Rank 1', 'source'),
(50001, 678,   'Holy Strike', 'Rank 2', 'source'),
(50002, 1866,  'Holy Strike', 'Rank 3', 'source'),
(50003, 680,   'Holy Strike', 'Rank 4', 'source'),
(50004, 2495,  'Holy Strike', 'Rank 5', 'source'),
(50005, 5569,  'Holy Strike', 'Rank 6', 'source'),
(50006, 10332, 'Holy Strike', 'Rank 7', 'source'),
(50007, 10333, 'Holy Strike', 'Rank 8', 'source'),
(50008, 2497,  'Divine Strike', 'Rank 1', 'source'),
(50009, 2498,  'Divine Strike', 'Rank 2', 'source'),
(50010, 2499,  'Divine Strike', 'Rank 3', 'source'),
(50011, 2500,  'Divine Strike', 'Rank 4', 'source'),
(50012, 2501,  'Divine Strike', 'Rank 5', 'source')
ON DUPLICATE KEY UPDATE `source_entry` = VALUES(`source_entry`), `spell_name` = VALUES(`spell_name`),
    `name_subtext` = VALUES(`name_subtext`), `icon_source` = VALUES(`icon_source`);
UPDATE `custom_spell_meta` SET `description` = 'An instant strike that hits all enemies within melee range, dealing weapon damage plus $s1 additional damage. All damage caused is considered holy damage.'
    WHERE `entry` BETWEEN 50008 AND 50012;

-- Applying this file alone does NOT deliver the icon/tooltip/wrapper fixes above to
-- players -- custom_spell_meta is only a staging table (see [[Spell Pages]]).
-- After running this file, call `POST http://localhost:5000/Patch/RebuildClientPatch`
-- against the running MangosSuperUI instance to actually rebuild patch-3.MPQ.

-- Divine Strike 'Invalid target' -- REAL root cause found 2026-08-28, after the
-- effectImplicitTargetB1=15 fix above (commit 903f99936) still didn't resolve it
-- live. Traced via temporary diagnostic logging (commit 1324a3cb3) at the strict
-- CheckCast() call in Spell::prepare(): every real cast attempt returned
-- SpellCastResult 10 = SPELL_FAILED_BAD_TARGETS ("Invalid target"), with a real
-- unit target present (hasUnitTarget=1) and no destination location (hasDest=0).
--
-- Root cause: Spell::ValidateExplicitTargetMask() -- an anti-cheat check that only
-- runs for client-initiated "strict" casts (m_isClientStarted), exactly why
-- Whirlwind's own inner sub-spell (15578) never hits it: that spell is always
-- server-triggered, never client-started. It compares what the CLIENT declares in
-- its cast packet against AllowedTargetMask, which the server computes purely from
-- effectImplicitTargetA/B (SpellMgr::GetAllowedTargetMask). Divine Strike's
-- effect1 uses effectImplicitTargetA1=22 (TARGET_LOCATION_CASTER_SRC) + B1=15
-- (TARGET_ENUM_UNITS_ENEMY_AOE_AT_SRC_LOC) -- both location/AOE target types,
-- contributing only TARGET_FLAG_SOURCE_LOCATION to AllowedTargetMask, no
-- TARGET_FLAG_UNIT.
--
-- But the CLIENT's own Spell.dbc copy of Divine Strike (40008-40012, cloned via
-- custom_spell_meta from Holy Strike's single-target template before the
-- "stun-to-AOE rework") was never updated to match -- confirmed by extracting the
-- live patch-3.MPQ and reading the raw client DBC fields directly: Divine
-- Strike's client-side effectImplicitTargetA[0] is still 6 (TARGET_UNIT_ENEMY),
-- identical to Holy Strike (40000). The client, correctly per its own (stale)
-- data, always includes the player's selected enemy in the cast packet
-- (TARGET_FLAG_UNIT) -- which the server's AllowedTargetMask (SOURCE_LOCATION
-- only) rejects outright.
--
-- Fix: rather than repatch the client DBC (a materially bigger, riskier change --
-- see tonight's mount-tooltip investigation for why editing target fields for
-- spells this pipeline has never touched before is not to be done lightly), add a
-- second, purely server-side effect that independently widens AllowedTargetMask
-- to also accept TARGET_FLAG_UNIT: effect2 = SPELL_EFFECT_DUMMY (3) with
-- effectImplicitTargetA2 = TARGET_UNIT_ENEMY (6). SPELL_EFFECT_DUMMY with no
-- spell-specific script registered (confirmed: no case for these IDs in
-- Spell::EffectDummy, no PetAura, no ScriptMgr hook) is a guaranteed no-op -- it
-- exists purely to make the server's own target-mask computation agree with what
-- the client, unavoidably, already sends. effect1 (the real weapon-damage AOE,
-- and the tooltip's $s1 reference) is untouched -- damage and tooltip both stay
-- exactly as they are. No client patch/rebuild needed -- this is the same "pure
-- spell_template data" category as the original effectImplicitTargetB1 fix.
UPDATE `spell_template` SET `effect2` = 3, `effectImplicitTargetA2` = 6
    WHERE `entry` IN (2497, 2498, 2499, 2500, 2501, 40008, 40009, 40010, 40011, 40012);
