-- ============================================================================
-- SuperUI-Core :: shared vmangos_admin prerequisites
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
-- This file also pre-creates MangosSuperUI's durable combat-loadout queue. The
-- core does not read that table during world load, but shipping it here keeps a
-- fresh SuperUI-Core + MangosSuperUI setup complete before the web app's first
-- boot. The web app owns runtime migrations for existing queue tables.
--
-- Additional admin tables (audit_log, config_history, feature registries, ...)
-- live in the MangosSuperUI repo and are created by its setup/startup paths.
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

-- One durable, replaceable combat-build intent per managed bot. A direct apply
-- is journaled here before TCP dispatch; an unsafe bot can retain one waiting
-- intent until it becomes eligible. Unknown outcomes remain `uncertain` and
-- are never retried blindly.
CREATE TABLE IF NOT EXISTS `vmangos_admin`.`bot_combat_loadout_queue` (
  `bot_guid` int(10) unsigned NOT NULL,
  `bot_name` varchar(32) NOT NULL,
  `queue_id` char(32) NOT NULL,
  `status` varchar(24) NOT NULL DEFAULT 'waiting',
  `payload_json` mediumtext NOT NULL,
  `spec_tab` tinyint(3) unsigned NOT NULL,
  `profile_id` varchar(63) NOT NULL,
  `profile_name` varchar(63) NOT NULL,
  `active_role` tinyint(3) unsigned NOT NULL,
  `active_role_name` varchar(32) NOT NULL,
  `rotation_mode` varchar(16) NOT NULL,
  `rotation_profile` varchar(63) DEFAULT NULL,
  `rotation_name` varchar(96) NOT NULL,
  `rotation_fingerprint` char(64) DEFAULT NULL,
  `reset_talents` tinyint(1) NOT NULL DEFAULT 0,
  `expected_revision` int(10) unsigned NOT NULL,
  `observed_session_at` datetime(3) DEFAULT NULL,
  `request_id` char(32) DEFAULT NULL,
  `claim_owner` char(32) DEFAULT NULL,
  `claim_expires_at` datetime(3) DEFAULT NULL,
  `attempt_count` int(10) unsigned NOT NULL DEFAULT 0,
  `queued_by` varchar(64) NOT NULL DEFAULT 'web',
  `queued_from` varchar(64) DEFAULT NULL,
  `created_at` datetime(3) NOT NULL DEFAULT current_timestamp(3),
  `updated_at` datetime(3) NOT NULL DEFAULT current_timestamp(3) ON UPDATE current_timestamp(3),
  `next_attempt_at` datetime(3) NOT NULL DEFAULT current_timestamp(3),
  `dispatched_at` datetime(3) DEFAULT NULL,
  `completed_at` datetime(3) DEFAULT NULL,
  `last_code` varchar(64) DEFAULT NULL,
  `last_message` text DEFAULT NULL,
  PRIMARY KEY (`bot_guid`),
  UNIQUE KEY `uq_bot_combat_loadout_queue_id` (`queue_id`),
  KEY `idx_bot_combat_loadout_queue_due` (`status`,`next_attempt_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
