-- Gives every new character a Dark Silk Shirt (entry 4333, Body slot) on creation,
-- via the stock playercreateinfo_item mechanism (no code change needed).
-- One row per valid race/class combo, pulled from playercreateinfo.

DELETE FROM `playercreateinfo_item` WHERE `itemid` = 4333;
INSERT INTO `playercreateinfo_item` (`race`, `class`, `itemid`, `amount`) VALUES
(1, 1, 4333, 1), (1, 2, 4333, 1), (1, 4, 4333, 1), (1, 5, 4333, 1), (1, 8, 4333, 1), (1, 9, 4333, 1),
(2, 1, 4333, 1), (2, 3, 4333, 1), (2, 4, 4333, 1), (2, 7, 4333, 1), (2, 9, 4333, 1),
(3, 1, 4333, 1), (3, 2, 4333, 1), (3, 3, 4333, 1), (3, 4, 4333, 1), (3, 5, 4333, 1),
(4, 1, 4333, 1), (4, 3, 4333, 1), (4, 4, 4333, 1), (4, 5, 4333, 1), (4, 11, 4333, 1),
(5, 1, 4333, 1), (5, 4, 4333, 1), (5, 5, 4333, 1), (5, 8, 4333, 1), (5, 9, 4333, 1),
(6, 1, 4333, 1), (6, 3, 4333, 1), (6, 7, 4333, 1), (6, 11, 4333, 1),
(7, 1, 4333, 1), (7, 4, 4333, 1), (7, 8, 4333, 1), (7, 9, 4333, 1),
(8, 1, 4333, 1), (8, 3, 4333, 1), (8, 4, 4333, 1), (8, 5, 4333, 1), (8, 7, 4333, 1), (8, 8, 4333, 1);
