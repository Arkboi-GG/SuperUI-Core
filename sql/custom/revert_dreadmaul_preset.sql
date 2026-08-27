-- Reverts the "Dreadmaul" instant-max-level character-creation preset back to
-- vanilla defaults, for NEW characters only. playercreateinfo* tables only
-- govern character CREATION, never touch already-existing characters.
-- Superseded scripts: sql/custom/repack/Custom-DREADMAUL_*.sql,
-- Custom-START_AT_DREADMAUL_HOLD.sql, Custom-START_AT_CAPITALS.sql (left in
-- place, untouched, purely for historical traceability -- no longer applied).
--
-- Does NOT touch: the Holy Strike/Divine Strike auto-learn mechanism
-- (ObjectMgr::LoadAutoLearnCustomSpells / Player::LearnAutoLearnCustomSpells --
-- pure C++/in-memory, no DB table backs it), the Dark Silk Shirt grant
-- (sql/custom/starting_shirt.sql, itemid 4333, untouched here), or the
-- Journeyman's Backpack grant (itemid 3914, untouched here except its
-- required_level fix below).

-- ============================================================
-- 1) Starting position/zone -> vanilla per-race spawn points
-- ============================================================
-- StartPlayerLevel already reset to 1 by the user (confirmed live in
-- mangosd.conf). Custom-START_AT_DREADMAUL_HOLD.sql then
-- Custom-START_AT_CAPITALS.sql overwrote every race's position/map/zone to
-- one of two blanket faction-capital spots -- restored here to the real
-- vanilla 1.12 starting zones (Northshire Valley, Valley of Trials, Coldridge
-- Valley, Shadowglen, Deathknell, Camp Narache), cross-verified against two
-- independent vanilla-era MaNGOS database sources.
UPDATE `playercreateinfo` SET `map`=0, `zone`=12, `position_x`=-8949.95, `position_y`=-132.493, `position_z`=83.5312, `orientation`=0 WHERE `race`=1; -- Human: Northshire Valley
UPDATE `playercreateinfo` SET `map`=1, `zone`=14, `position_x`=-618.518, `position_y`=-4251.67, `position_z`=38.718, `orientation`=0 WHERE `race`=2; -- Orc: Valley of Trials
UPDATE `playercreateinfo` SET `map`=0, `zone`=1, `position_x`=-6240.32, `position_y`=331.033, `position_z`=382.758, `orientation`=6.17716 WHERE `race`=3; -- Dwarf: Coldridge Valley
UPDATE `playercreateinfo` SET `map`=1, `zone`=141, `position_x`=10311.3, `position_y`=832.463, `position_z`=1326.41, `orientation`=5.69632 WHERE `race`=4; -- Night Elf: Shadowglen
UPDATE `playercreateinfo` SET `map`=0, `zone`=85, `position_x`=1676.71, `position_y`=1678.31, `position_z`=121.67, `orientation`=2.70526 WHERE `race`=5; -- Undead: Deathknell
UPDATE `playercreateinfo` SET `map`=1, `zone`=215, `position_x`=-2917.58, `position_y`=-257.98, `position_z`=52.9968, `orientation`=0 WHERE `race`=6; -- Tauren: Camp Narache
UPDATE `playercreateinfo` SET `map`=0, `zone`=1, `position_x`=-6240.32, `position_y`=331.033, `position_z`=382.758, `orientation`=0 WHERE `race`=7; -- Gnome: Coldridge Valley
UPDATE `playercreateinfo` SET `map`=1, `zone`=14, `position_x`=-618.518, `position_y`=-4251.67, `position_z`=38.718, `orientation`=0 WHERE `race`=8; -- Troll: Valley of Trials

-- ============================================================
-- 2) Starting spells -> remove every Dreadmaul-era grant
-- ============================================================
-- All four Dreadmaul spell scripts (SPELLBOOK_49/60, FIRSTAID_FIX,
-- RIDING_FIRSTAID/RIDING_60, WEAPON_SKILLS_60) used INSERT IGNORE, never
-- DELETE FROM the whole table -- every genuinely pre-existing vanilla row
-- (weapon proficiencies a race/class combo already had, racial/language/
-- passive spells, etc.) was left completely untouched throughout. Confirmed
-- live: `note LIKE 'trainer curriculum%'` matches zero rows (that insert's
-- JOIN produced no new rows in practice) and most `quest: %` grants were also
-- silently ignored where the vanilla row already existed (e.g. Redemption/
-- Sense Undead are genuine early Paladin spells, never touched). The specific
-- notes below are the only patterns confirmed, live, to uniquely identify
-- Dreadmaul's actual additions -- safe to delete by note text alone, nothing
-- else in this table is touched.
DELETE FROM `playercreateinfo_spell` WHERE `note` LIKE 'weapon proficiency: %'; -- WEAPON_SKILLS_60: every eligible weapon skill, not just the vanilla starting subset
DELETE FROM `playercreateinfo_spell` WHERE `note` = 'Journeyman riding skill (epic mount unlock)'; -- RIDING_60: spell 33391, vanilla level 1 has no riding at all
DELETE FROM `playercreateinfo_spell` WHERE `note` = 'First Aid'; -- RIDING_FIRSTAID/FIRSTAID_FIX: spell 3273, vanilla level 1 has no First Aid
DELETE FROM `playercreateinfo_spell` WHERE `note` LIKE 'quest: %'; -- SPELLBOOK_49/60: quest-reward spells (mounts, totems, warlock pets) pre-granted instead of earned by questing

-- ============================================================
-- 3) Starting items -> vanilla per-race/class loadout
-- ============================================================
-- Custom-DREADMAUL_LOADOUTS_49.sql opened with `DELETE FROM playercreateinfo_item;`
-- (full wipe, no backup table exists anywhere in this DB) before rebuilding
-- from scratch, so nothing pre-Dreadmaul survives in this table today except
-- by pure coincidence (e.g. the Hearthstone, itemid 6948, which vanilla also
-- grants). Cannot diff against a live "before" state -- reconstructed instead
-- from this repo's own git history: sql/old_migrations/20190122024303_world.sql
-- is the actual original vanilla seed for this exact database (all 40 valid
-- race/class combos, committed to git years before Dreadmaul existed), plus
-- sql/old_migrations/20200721074812_world.sql's rogue thrown-weapon amount
-- fix (a genuine pre-Dreadmaul correction, not part of the preset, reapplied
-- below so it isn't lost).
--
-- Preserves itemid 4333 (Dark Silk Shirt, sql/custom/starting_shirt.sql) and
-- itemid 3914 (Journeyman's Backpack, Custom-DREADMAUL_STARTING_BAGS.sql --
-- kept granting per explicit instruction, not reverted) by simply never
-- touching those two itemids.
DELETE FROM `playercreateinfo_item` WHERE `itemid` NOT IN (4333, 3914);

INSERT INTO `playercreateinfo_item` (`race`, `class`, `itemid`, `amount`) VALUES
(1, 1, 25, 1), (1, 1, 38, 1), (1, 1, 39, 1), (1, 1, 40, 1), (1, 1, 117, 4), (1, 1, 2362, 1), (1, 1, 6948, 1),
(1, 2, 43, 1), (1, 2, 44, 1), (1, 2, 45, 1), (1, 2, 159, 2), (1, 2, 2070, 4), (1, 2, 2361, 1), (1, 2, 6948, 1),
(1, 4, 47, 1), (1, 4, 48, 1), (1, 4, 49, 1), (1, 4, 2070, 4), (1, 4, 2092, 1), (1, 4, 2947, 200), (1, 4, 6948, 1),
(1, 5, 36, 1), (1, 5, 51, 1), (1, 5, 52, 1), (1, 5, 53, 1), (1, 5, 159, 2), (1, 5, 2070, 4), (1, 5, 6098, 1), (1, 5, 6948, 1),
(1, 8, 35, 1), (1, 8, 55, 1), (1, 8, 56, 1), (1, 8, 159, 2), (1, 8, 1395, 1), (1, 8, 2070, 4), (1, 8, 6096, 1), (1, 8, 6948, 1),
(1, 9, 57, 1), (1, 9, 59, 1), (1, 9, 159, 2), (1, 9, 1396, 1), (1, 9, 2092, 1), (1, 9, 4604, 4), (1, 9, 6097, 1), (1, 9, 6948, 1),
(2, 1, 117, 4), (2, 1, 139, 1), (2, 1, 140, 1), (2, 1, 6125, 1), (2, 1, 6948, 1), (2, 1, 12282, 1),
(2, 3, 37, 1), (2, 3, 117, 4), (2, 3, 127, 1), (2, 3, 159, 2), (2, 3, 2101, 1), (2, 3, 2504, 1), (2, 3, 2512, 200), (2, 3, 6126, 1), (2, 3, 6127, 1), (2, 3, 6948, 1),
(2, 4, 117, 4), (2, 4, 120, 1), (2, 4, 121, 1), (2, 4, 2092, 1), (2, 4, 2105, 1), (2, 4, 3111, 200), (2, 4, 6948, 1),
(2, 7, 36, 1), (2, 7, 117, 4), (2, 7, 153, 1), (2, 7, 154, 1), (2, 7, 159, 2), (2, 7, 6948, 1),
(2, 9, 59, 1), (2, 9, 117, 4), (2, 9, 159, 2), (2, 9, 1396, 1), (2, 9, 2092, 1), (2, 9, 6129, 1), (2, 9, 6948, 1),
(3, 1, 38, 1), (3, 1, 39, 1), (3, 1, 40, 1), (3, 1, 117, 4), (3, 1, 6948, 1), (3, 1, 12282, 1),
(3, 2, 43, 1), (3, 2, 159, 2), (3, 2, 2361, 1), (3, 2, 4540, 4), (3, 2, 6117, 1), (3, 2, 6118, 1), (3, 2, 6948, 1),
(3, 3, 37, 1), (3, 3, 117, 4), (3, 3, 129, 1), (3, 3, 147, 1), (3, 3, 148, 1), (3, 3, 159, 2), (3, 3, 2102, 1), (3, 3, 2508, 1), (3, 3, 2516, 200), (3, 3, 6948, 1),
(3, 4, 47, 1), (3, 4, 48, 1), (3, 4, 49, 1), (3, 4, 2092, 1), (3, 4, 3111, 200), (3, 4, 4540, 4), (3, 4, 6948, 1),
(3, 5, 36, 1), (3, 5, 51, 1), (3, 5, 52, 1), (3, 5, 53, 1), (3, 5, 159, 2), (3, 5, 4540, 4), (3, 5, 6098, 1), (3, 5, 6948, 1),
(3, 8, 35, 1), (3, 8, 55, 1), (3, 8, 159, 2), (3, 8, 1395, 1), (3, 8, 4540, 4), (3, 8, 6096, 1), (3, 8, 6116, 1), (3, 8, 6948, 1),
(4, 1, 25, 1), (4, 1, 117, 4), (4, 1, 2362, 1), (4, 1, 6120, 1), (4, 1, 6121, 1), (4, 1, 6122, 1), (4, 1, 6948, 1),
(4, 3, 117, 4), (4, 3, 129, 1), (4, 3, 147, 1), (4, 3, 148, 1), (4, 3, 159, 2), (4, 3, 2092, 1), (4, 3, 2101, 1), (4, 3, 2504, 1), (4, 3, 2512, 200), (4, 3, 6948, 1),
(4, 4, 47, 1), (4, 4, 48, 1), (4, 4, 49, 1), (4, 4, 2092, 1), (4, 4, 2947, 200), (4, 4, 4540, 4), (4, 4, 6948, 1),
(4, 5, 36, 1), (4, 5, 51, 1), (4, 5, 52, 1), (4, 5, 53, 1), (4, 5, 159, 2), (4, 5, 2070, 4), (4, 5, 6119, 1), (4, 5, 6948, 1),
(4, 11, 159, 2), (4, 11, 3661, 1), (4, 11, 4536, 4), (4, 11, 6123, 1), (4, 11, 6124, 1), (4, 11, 6948, 1),
(5, 1, 25, 1), (5, 1, 139, 1), (5, 1, 140, 1), (5, 1, 2362, 1), (5, 1, 4604, 4), (5, 1, 6125, 1), (5, 1, 6948, 1),
(5, 4, 120, 1), (5, 4, 121, 1), (5, 4, 2092, 1), (5, 4, 2105, 1), (5, 4, 2947, 200), (5, 4, 4604, 4), (5, 4, 6948, 1),
(5, 5, 36, 1), (5, 5, 51, 1), (5, 5, 52, 1), (5, 5, 53, 1), (5, 5, 159, 2), (5, 5, 4604, 4), (5, 5, 6144, 1), (5, 5, 6948, 1),
(5, 8, 35, 1), (5, 8, 55, 1), (5, 8, 159, 2), (5, 8, 1395, 1), (5, 8, 4604, 4), (5, 8, 6096, 1), (5, 8, 6140, 1), (5, 8, 6948, 1),
(5, 9, 59, 1), (5, 9, 159, 2), (5, 9, 1396, 1), (5, 9, 2092, 1), (5, 9, 4604, 4), (5, 9, 6129, 1), (5, 9, 6948, 1),
(6, 1, 139, 1), (6, 1, 2361, 1), (6, 1, 4540, 4), (6, 1, 6125, 1), (6, 1, 6948, 1),
(6, 3, 37, 1), (6, 3, 117, 4), (6, 3, 127, 1), (6, 3, 159, 2), (6, 3, 2102, 1), (6, 3, 2508, 1), (6, 3, 2516, 200), (6, 3, 6126, 1), (6, 3, 6948, 1),
(6, 7, 36, 1), (6, 7, 153, 1), (6, 7, 154, 1), (6, 7, 159, 2), (6, 7, 4604, 4), (6, 7, 6948, 1),
(6, 11, 35, 1), (6, 11, 159, 2), (6, 11, 4536, 4), (6, 11, 6124, 1), (6, 11, 6139, 1), (6, 11, 6948, 1),
(7, 1, 25, 1), (7, 1, 38, 1), (7, 1, 39, 1), (7, 1, 40, 1), (7, 1, 117, 4), (7, 1, 2362, 1), (7, 1, 6948, 1),
(7, 4, 47, 1), (7, 4, 48, 1), (7, 4, 49, 1), (7, 4, 117, 4), (7, 4, 2092, 1), (7, 4, 2947, 200), (7, 4, 6948, 1),
(7, 8, 35, 1), (7, 8, 55, 1), (7, 8, 56, 1), (7, 8, 159, 2), (7, 8, 1395, 1), (7, 8, 4536, 4), (7, 8, 6096, 1), (7, 8, 6948, 1),
(7, 9, 57, 1), (7, 9, 59, 1), (7, 9, 159, 2), (7, 9, 1396, 1), (7, 9, 2092, 1), (7, 9, 4604, 4), (7, 9, 6097, 1), (7, 9, 6948, 1),
(8, 1, 37, 1), (8, 1, 117, 4), (8, 1, 139, 1), (8, 1, 2362, 1), (8, 1, 3111, 200), (8, 1, 6125, 1), (8, 1, 6948, 1),
(8, 3, 37, 1), (8, 3, 127, 1), (8, 3, 159, 2), (8, 3, 2101, 1), (8, 3, 2504, 1), (8, 3, 2512, 200), (8, 3, 4604, 4), (8, 3, 6126, 1), (8, 3, 6948, 1),
(8, 4, 117, 4), (8, 4, 2092, 1), (8, 4, 3111, 200), (8, 4, 6136, 1), (8, 4, 6137, 1), (8, 4, 6138, 1), (8, 4, 6948, 1),
(8, 5, 36, 1), (8, 5, 52, 1), (8, 5, 53, 1), (8, 5, 159, 2), (8, 5, 4540, 4), (8, 5, 6144, 1), (8, 5, 6948, 1),
(8, 7, 36, 1), (8, 7, 117, 4), (8, 7, 159, 2), (8, 7, 6134, 1), (8, 7, 6135, 1), (8, 7, 6948, 1),
(8, 8, 35, 1), (8, 8, 55, 1), (8, 8, 117, 4), (8, 8, 159, 2), (8, 8, 1395, 1), (8, 8, 6096, 1), (8, 8, 6140, 1), (8, 8, 6948, 1);

-- Reapply the genuine (non-Dreadmaul) rogue thrown-weapon amount fix from
-- sql/old_migrations/20200721074812_world.sql, lost in the full wipe above.
UPDATE `playercreateinfo_item` SET `amount` = 100 WHERE `class` = 4 AND `itemid` IN (2947, 3111);

-- Bag required_level fix: was gated to the old instant-max-level design;
-- drop to 1 so a fresh level-1 character can equip/use it immediately.
UPDATE `item_template` SET `required_level` = 1 WHERE `entry` = 3914;

-- ============================================================
-- 4) Gearing lockboxes: convert the old auto-EQUIPPED set into a new,
--    ungranted lockbox per class -- same construct as the existing
--    alt-spec lockboxes (40100-40105, Custom-DREADMAUL_GEARING_LOCKBOXES_60.sql),
--    which are explicitly KEPT as-is (item_template/item_loot_template
--    untouched) even though nothing grants them either. New entries
--    40106-40114, one per class that had gear auto-equipped -- NOT inserted
--    into playercreateinfo_item; the user has a future plan for handing all
--    of these out. Item lists reconstructed from the actual live
--    playercreateinfo_item state before this migration (cross-checked
--    against Custom-DREADMAUL_LOADOUTS_60.sql's own item lists -- this catches
--    2 items that file's own comments never mentioned keeping: 13071 for
--    Warrior, 17713 for Paladin, both real gear pieces).
--
--    Hunter's set is named plainly "Hunter" (not "Hunter Marksmanship") per
--    user decision -- Custom-DREADMAUL_LOADOUTS_60.sql's own comment says the
--    itemization is "effectively spec-agnostic for vanilla Hunters" and
--    covers Beast Mastery too, so a spec-qualified name would be misleading.
DELETE FROM `item_template` WHERE `entry` BETWEEN 40106 AND 40114;
INSERT INTO `item_template`
(entry, patch, class, subclass, name, display_id, quality, flags, buy_count, buy_price, sell_price,
 inventory_type, allowable_class, allowable_race, item_level, required_level, max_count, stackable,
 bonding, lock_id)
VALUES
(40106, 10, 15, 0, "Warrior Protection",  9632, 3, 4, 1, 0, 0, 0, -1, -1, 60, 0, 1, 1, 1, 0),
(40107, 10, 15, 0, "Paladin Retribution", 9632, 3, 4, 1, 0, 0, 0, -1, -1, 60, 0, 1, 1, 1, 0),
(40108, 10, 15, 0, "Hunter",              9632, 3, 4, 1, 0, 0, 0, -1, -1, 60, 0, 1, 1, 1, 0),
(40109, 10, 15, 0, "Rogue Combat",        9632, 3, 4, 1, 0, 0, 0, -1, -1, 60, 0, 1, 1, 1, 0),
(40110, 10, 15, 0, "Priest Shadow",       9632, 3, 4, 1, 0, 0, 0, -1, -1, 60, 0, 1, 1, 1, 0),
(40111, 10, 15, 0, "Shaman Elemental",    9632, 3, 4, 1, 0, 0, 0, -1, -1, 60, 0, 1, 1, 1, 0),
(40112, 10, 15, 0, "Mage Frost",          9632, 3, 4, 1, 0, 0, 0, -1, -1, 60, 0, 1, 1, 1, 0),
(40113, 10, 15, 0, "Warlock Destruction", 9632, 3, 4, 1, 0, 0, 0, -1, -1, 60, 0, 1, 1, 1, 0),
(40114, 10, 15, 0, "Druid Balance",       9632, 3, 4, 1, 0, 0, 0, -1, -1, 60, 0, 1, 1, 1, 0);

DELETE FROM `item_loot_template` WHERE `entry` BETWEEN 40106 AND 40114;
INSERT INTO `item_loot_template` (entry, item, ChanceOrQuestChance, groupid, mincountOrRef, maxcount, condition_id, patch_min, patch_max) VALUES
-- 40106 Warrior Protection
(40106,11726,100,0,1,1,0,0,10),(40106,11815,100,0,1,1,0,0,10),(40106,12602,100,0,1,1,0,0,10),(40106,12640,100,0,1,1,0,0,10),
(40106,12936,100,0,1,1,0,0,10),(40106,12940,100,0,1,1,0,0,10),(40106,13071,100,0,1,1,0,0,10),(40106,13098,100,0,1,1,0,0,10),
(40106,13142,100,0,1,1,0,0,10),(40106,13397,100,0,1,1,0,0,10),(40106,13965,100,0,1,1,0,0,10),(40106,14616,100,0,1,1,0,0,10),
(40106,15411,100,0,1,1,0,0,10),(40106,16732,100,0,1,1,0,0,10),(40106,16733,100,0,1,1,0,0,10),(40106,17713,100,0,1,1,0,0,10),
-- 40107 Paladin Retribution
(40107,11726,100,0,1,1,0,0,10),(40107,11815,100,0,1,1,0,0,10),(40107,12587,100,0,1,1,0,0,10),(40107,12784,100,0,1,1,0,0,10),
(40107,12927,100,0,1,1,0,0,10),(40107,13098,100,0,1,1,0,0,10),(40107,13340,100,0,1,1,0,0,10),(40107,13400,100,0,1,1,0,0,10),
(40107,13959,100,0,1,1,0,0,10),(40107,13965,100,0,1,1,0,0,10),(40107,14616,100,0,1,1,0,0,10),(40107,15062,100,0,1,1,0,0,10),
(40107,15063,100,0,1,1,0,0,10),(40107,15411,100,0,1,1,0,0,10),(40107,17713,100,0,1,1,0,0,10),
-- 40108 Hunter (includes 1000 Jagged Arrow ammo, matching the original grant quantity)
(40108,11726,100,0,1,1,0,0,10),(40108,11815,100,0,1,1,0,0,10),(40108,12634,100,0,1,1,0,0,10),(40108,12651,100,0,1,1,0,0,10),
(40108,12927,100,0,1,1,0,0,10),(40108,13098,100,0,1,1,0,0,10),(40108,13211,100,0,1,1,0,0,10),(40108,13340,100,0,1,1,0,0,10),
(40108,13404,100,0,1,1,0,0,10),(40108,13965,100,0,1,1,0,0,10),(40108,13967,100,0,1,1,0,0,10),(40108,15062,100,0,1,1,0,0,10),
(40108,15063,100,0,1,1,0,0,10),(40108,15411,100,0,1,1,0,0,10),(40108,17713,100,0,1,1,0,0,10),(40108,18725,100,0,1,1,0,0,10),
(40108,11285,100,0,1000,1000,0,0,10),
-- 40109 Rogue Combat
(40109,11815,100,0,1,1,0,0,10),(40109,12553,100,0,1,1,0,0,10),(40109,12651,100,0,1,1,0,0,10),(40109,12927,100,0,1,1,0,0,10),
(40109,12939,100,0,1,1,0,0,10),(40109,12940,100,0,1,1,0,0,10),(40109,13098,100,0,1,1,0,0,10),(40109,13252,100,0,1,1,0,0,10),
(40109,13340,100,0,1,1,0,0,10),(40109,13404,100,0,1,1,0,0,10),(40109,13965,100,0,1,1,0,0,10),(40109,14637,100,0,1,1,0,0,10),
(40109,15062,100,0,1,1,0,0,10),(40109,15063,100,0,1,1,0,0,10),(40109,15411,100,0,1,1,0,0,10),(40109,16710,100,0,1,1,0,0,10),
(40109,17713,100,0,1,1,0,0,10),
-- 40110 Priest Shadow
(40110,10461,100,0,1,1,0,0,10),(40110,11623,100,0,1,1,0,0,10),(40110,11662,100,0,1,1,0,0,10),(40110,11824,100,0,1,1,0,0,10),
(40110,11904,100,0,1,1,0,0,10),(40110,12545,100,0,1,1,0,0,10),(40110,12930,100,0,1,1,0,0,10),(40110,13170,100,0,1,1,0,0,10),
(40110,13253,100,0,1,1,0,0,10),(40110,13349,100,0,1,1,0,0,10),(40110,13396,100,0,1,1,0,0,10),(40110,13968,100,0,1,1,0,0,10),
(40110,14112,100,0,1,1,0,0,10),(40110,14136,100,0,1,1,0,0,10),(40110,18691,100,0,1,1,0,0,10),(40110,18727,100,0,1,1,0,0,10),
(40110,18735,100,0,1,1,0,0,10),
-- 40111 Shaman Elemental
(40111,11623,100,0,1,1,0,0,10),(40111,11662,100,0,1,1,0,0,10),(40111,11822,100,0,1,1,0,0,10),(40111,11824,100,0,1,1,0,0,10),
(40111,11904,100,0,1,1,0,0,10),(40111,11962,100,0,1,1,0,0,10),(40111,12545,100,0,1,1,0,0,10),(40111,12624,100,0,1,1,0,0,10),
(40111,12930,100,0,1,1,0,0,10),(40111,13170,100,0,1,1,0,0,10),(40111,13253,100,0,1,1,0,0,10),(40111,13964,100,0,1,1,0,0,10),
(40111,13968,100,0,1,1,0,0,10),(40111,18681,100,0,1,1,0,0,10),(40111,18727,100,0,1,1,0,0,10),(40111,22403,100,0,1,1,0,0,10),
-- 40112 Mage Frost
(40112,11623,100,0,1,1,0,0,10),(40112,11662,100,0,1,1,0,0,10),(40112,11782,100,0,1,1,0,0,10),(40112,11822,100,0,1,1,0,0,10),
(40112,11824,100,0,1,1,0,0,10),(40112,11904,100,0,1,1,0,0,10),(40112,11962,100,0,1,1,0,0,10),(40112,12545,100,0,1,1,0,0,10),
(40112,13170,100,0,1,1,0,0,10),(40112,13253,100,0,1,1,0,0,10),(40112,13938,100,0,1,1,0,0,10),(40112,13964,100,0,1,1,0,0,10),
(40112,13965,100,0,1,1,0,0,10),(40112,13968,100,0,1,1,0,0,10),(40112,14152,100,0,1,1,0,0,10),(40112,18727,100,0,1,1,0,0,10),
(40112,22403,100,0,1,1,0,0,10),
-- 40113 Warlock Destruction
(40113,10461,100,0,1,1,0,0,10),(40113,11623,100,0,1,1,0,0,10),(40113,11662,100,0,1,1,0,0,10),(40113,11824,100,0,1,1,0,0,10),
(40113,11904,100,0,1,1,0,0,10),(40113,12545,100,0,1,1,0,0,10),(40113,12930,100,0,1,1,0,0,10),(40113,13170,100,0,1,1,0,0,10),
(40113,13253,100,0,1,1,0,0,10),(40113,13396,100,0,1,1,0,0,10),(40113,13964,100,0,1,1,0,0,10),(40113,13968,100,0,1,1,0,0,10),
(40113,14112,100,0,1,1,0,0,10),(40113,14153,100,0,1,1,0,0,10),(40113,18727,100,0,1,1,0,0,10),(40113,18735,100,0,1,1,0,0,10),
(40113,22403,100,0,1,1,0,0,10),
-- 40114 Druid Balance
(40114,11623,100,0,1,1,0,0,10),(40114,11662,100,0,1,1,0,0,10),(40114,11822,100,0,1,1,0,0,10),(40114,11824,100,0,1,1,0,0,10),
(40114,11904,100,0,1,1,0,0,10),(40114,11924,100,0,1,1,0,0,10),(40114,11962,100,0,1,1,0,0,10),(40114,12545,100,0,1,1,0,0,10),
(40114,12930,100,0,1,1,0,0,10),(40114,13170,100,0,1,1,0,0,10),(40114,13253,100,0,1,1,0,0,10),(40114,13964,100,0,1,1,0,0,10),
(40114,13968,100,0,1,1,0,0,10),(40114,18681,100,0,1,1,0,0,10),(40114,18727,100,0,1,1,0,0,10),(40114,22403,100,0,1,1,0,0,10);

SELECT race, class, map, zone FROM playercreateinfo ORDER BY race, class;
SELECT COUNT(*) AS spell_rows FROM playercreateinfo_spell;
SELECT COUNT(*) AS item_rows FROM playercreateinfo_item;
SELECT itemid, COUNT(*) FROM playercreateinfo_item GROUP BY itemid ORDER BY itemid;
SELECT entry, name FROM item_template WHERE entry BETWEEN 40106 AND 40114 ORDER BY entry;
SELECT entry, COUNT(*) FROM item_loot_template WHERE entry BETWEEN 40106 AND 40114 GROUP BY entry ORDER BY entry;
