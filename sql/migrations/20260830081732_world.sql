DROP PROCEDURE IF EXISTS add_migration;
DELIMITER ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260830081732');
IF v = 0 THEN
INSERT INTO `migrations` VALUES ('20260830081732');
-- Add your query below.

-- GOA: insert ITEM_QUALITY_REFORGED (5) and ITEM_QUALITY_RELIC (8), shifting
-- Legendary 5->6 and Artifact 6->7. Bump Artifact before Legendary so the two
-- UPDATEs can't collide.
UPDATE `item_template` SET `Quality` = 7 WHERE `Quality` = 6;
UPDATE `item_template` SET `Quality` = 6 WHERE `Quality` = 5;

-- End of migration.
END IF;
END??
DELIMITER ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
