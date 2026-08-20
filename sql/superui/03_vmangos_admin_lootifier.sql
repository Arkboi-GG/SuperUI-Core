-- ============================================================================
-- SuperUI-Core :: vmangos_admin.lootifier_generated_items  (BOOT-CRITICAL)
-- ============================================================================
-- Not a core game DB, but the compiled core reads it while loading the world:
--
--   Loading quest reward variants...
--   SQL: SELECT base_entry, generated_entry, budget_pct
--        FROM `vmangos_admin`.`lootifier_generated_items` WHERE creature_entry = 0
--
-- If the table is absent, mangosd aborts with:
--   [1146] Table 'vmangos_admin.lootifier_generated_items' doesn't exist
--   Your database structure is not up to date...
--
-- On a fresh install this must exist BEFORE mangosd's first boot — the
-- MangosSuperUI web app that would otherwise create it has not run yet.
--
-- This creates only the one table the core needs. The full admin schema
-- (audit_log, config_history, scheduled_actions, lootifier_loot_entries, ...)
-- lives in the MangosSuperUI repo (MangosSuperUI/sql/vmangos_admin_schema.sql)
-- and is created by that web app's DbInitializationService on first boot;
-- loading the full file here
-- instead is fine and leaves nothing for the app to bootstrap.
-- ----------------------------------------------------------------------------

CREATE DATABASE IF NOT EXISTS `vmangos_admin`
  DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

CREATE TABLE IF NOT EXISTS `vmangos_admin`.`lootifier_generated_items` (
  `id`              int(11) NOT NULL AUTO_INCREMENT,
  `generated_entry` int(11) NOT NULL,
  `base_entry`      int(11) NOT NULL,
  `creature_entry`  int(11) NOT NULL,
  `budget_pct`      float DEFAULT 0,
  `tier_name`       varchar(64) DEFAULT '',
  `created_at`      datetime NOT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_creature` (`creature_entry`),
  KEY `idx_generated` (`generated_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
