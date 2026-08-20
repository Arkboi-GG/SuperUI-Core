-- ============================================================================
-- SuperUI-Core :: characters.superui_*  (RTS World Editor)
-- ============================================================================
-- The RTS / World-State system persists its rules and live state in the
-- `characters` database. The MangosSuperUI web app seeds these during the RTS
-- world-creation ceremony (MangosSuperUI/Services/RtsWorldCreationService.cs:194-202),
-- and the compiled core's world-state/hero logic reads them at runtime.
--
-- DDL below is verbatim from RtsWorldCreationService.BuildCharactersSeedSql().
-- Run against the `characters` database.
-- ----------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `superui_worldstate` (
  `key` VARCHAR(32) NOT NULL PRIMARY KEY,
  `value` VARCHAR(64) NOT NULL
);

CREATE TABLE IF NOT EXISTS `superui_rules_zone` (
  `zone_id` INT UNSIGNED NOT NULL PRIMARY KEY,
  `ore` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `skins` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `herbs` TINYINT UNSIGNED NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS `superui_rules_hub` (
  `hub_id` SMALLINT UNSIGNED NOT NULL PRIMARY KEY,
  `zone_id` INT UNSIGNED NOT NULL,
  `name` VARCHAR(64) NOT NULL,
  `banner_go_guid` INT UNSIGNED NOT NULL,
  `event_alliance` SMALLINT UNSIGNED NOT NULL,
  `event_horde` SMALLINT UNSIGNED NOT NULL,
  `capture_ms` INT UNSIGNED NOT NULL DEFAULT 60000,
  `initial_controller` TINYINT UNSIGNED NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS `superui_rules_hero` (
  `hero_level` TINYINT UNSIGNED NOT NULL PRIMARY KEY,
  `declare_cost` INT UNSIGNED NOT NULL,
  `revive_fee` INT UNSIGNED NOT NULL,
  `spell_id` INT UNSIGNED NOT NULL,
  `scale_percent` SMALLINT UNSIGNED NOT NULL DEFAULT 100,
  `damage_percent` SMALLINT UNSIGNED NOT NULL DEFAULT 100
);

CREATE TABLE IF NOT EXISTS `superui_rules_dungeon` (
  `map_id` INT UNSIGNED NOT NULL PRIMARY KEY,
  `final_boss_entry` INT UNSIGNED NOT NULL,
  `buff_spell_id` INT UNSIGNED NOT NULL,
  `loot_items` TINYINT UNSIGNED NOT NULL DEFAULT 10
);

CREATE TABLE IF NOT EXISTS `superui_faction` (
  `team` TINYINT UNSIGNED NOT NULL PRIMARY KEY,
  `honor_pool` BIGINT NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS `superui_heroes` (
  `guid` INT UNSIGNED NOT NULL PRIMARY KEY,
  `team` TINYINT UNSIGNED NOT NULL,
  `hero_level` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `dead` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `declared_at` BIGINT UNSIGNED NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS `superui_zone_control` (
  `zone_id` INT UNSIGNED NOT NULL PRIMARY KEY,
  `controller` TINYINT UNSIGNED NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS `superui_dungeon_control` (
  `map_id` INT UNSIGNED NOT NULL PRIMARY KEY,
  `controller` TINYINT UNSIGNED NOT NULL DEFAULT 0
);
