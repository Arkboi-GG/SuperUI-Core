DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260825120000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260825120000');
-- Add your query below.

-- Persist a stable class-local talent profile separately from the bot's
-- current combat role. Each guard makes this safe when MangosSuperUI's
-- startup schema self-heal created either column first.
IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
               WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='playerbot' AND COLUMN_NAME='spec_tab') THEN
  ALTER TABLE `playerbot`
    ADD COLUMN `spec_tab` tinyint(3) unsigned NOT NULL DEFAULT '255'
    COMMENT 'Class-local talent profile 0..2; 255 means unassigned' AFTER `class`;
END IF;
IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
               WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='playerbot' AND COLUMN_NAME='active_role') THEN
  ALTER TABLE `playerbot`
    ADD COLUMN `active_role` tinyint(3) unsigned NOT NULL DEFAULT '0'
    COMMENT 'CombatBotRoles: 0 unassigned/invalid, 1 melee, 2 ranged, 3 tank, 4 healer' AFTER `spec_tab`;
END IF;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
