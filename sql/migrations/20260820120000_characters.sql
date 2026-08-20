DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260820120000');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260820120000');
-- Add your query below.

-- playerbot: add the 8 columns the compiled PlayerBotMgr SELECTs at startup
--   SELECT char_guid, chance, ai, race, class, level, map,
--          position_x, position_y, position_z, name FROM playerbot
-- The base characters.sql historically created only (char_guid, chance,
-- comment, ai); without the rest, mangosd terminates at "Loading Bots".
-- Each ADD is guarded against information_schema so this migration is safe
-- whether the columns are absent (fresh DB) or were already added at runtime
-- by the MangosSuperUI web app (BotBrainService.EnsurePlayerbotColumnsAsync).

IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='playerbot' AND COLUMN_NAME='race') THEN
  ALTER TABLE `playerbot` ADD COLUMN `race` tinyint(3) unsigned NOT NULL DEFAULT '0' AFTER `ai`;
END IF;
IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='playerbot' AND COLUMN_NAME='class') THEN
  ALTER TABLE `playerbot` ADD COLUMN `class` tinyint(3) unsigned NOT NULL DEFAULT '0' AFTER `race`;
END IF;
IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='playerbot' AND COLUMN_NAME='level') THEN
  ALTER TABLE `playerbot` ADD COLUMN `level` tinyint(3) unsigned NOT NULL DEFAULT '0' AFTER `class`;
END IF;
IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='playerbot' AND COLUMN_NAME='map') THEN
  ALTER TABLE `playerbot` ADD COLUMN `map` smallint(5) unsigned NOT NULL DEFAULT '0' COMMENT 'Map Identifier' AFTER `level`;
END IF;
IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='playerbot' AND COLUMN_NAME='position_x') THEN
  ALTER TABLE `playerbot` ADD COLUMN `position_x` float NOT NULL DEFAULT '0' AFTER `map`;
END IF;
IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='playerbot' AND COLUMN_NAME='position_y') THEN
  ALTER TABLE `playerbot` ADD COLUMN `position_y` float NOT NULL DEFAULT '0' AFTER `position_x`;
END IF;
IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='playerbot' AND COLUMN_NAME='position_z') THEN
  ALTER TABLE `playerbot` ADD COLUMN `position_z` float NOT NULL DEFAULT '0' AFTER `position_y`;
END IF;
IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='playerbot' AND COLUMN_NAME='name') THEN
  ALTER TABLE `playerbot` ADD COLUMN `name` varchar(12) NOT NULL DEFAULT '' AFTER `position_z`;
END IF;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
