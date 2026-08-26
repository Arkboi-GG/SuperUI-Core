-- Custom Enchant NPC: gossip-driven gear enchanting (Helmet/Shoulders/Cloak/Chest/
-- Bracer/Gloves/Pants/Boots/Weapon), backed by src/scripts/custom/custom_creatures.cpp
-- (GossipHello_EnchantNPC / GossipSelect_EnchantNPC, ScriptName 'custom_enchant_npc').
-- Not spawned by this file on purpose -- only the template is inserted; spawn it
-- in-game with `.npc add 190000` wherever/whenever you're ready.

DELETE FROM `creature_template` WHERE `entry` = 190000;
INSERT INTO `creature_template`
    (`entry`, `patch`, `name`, `subname`, `level_min`, `level_max`, `faction`, `npc_flags`,
     `gossip_menu_id`, `display_id1`, `unit_class`, `civilian`, `movement_type`, `inhabit_type`,
     `ai_name`, `script_name`)
VALUES
    (190000, 0, 'Elowen Duskweave', 'Enchanter', 60, 60, 120, 1,
     0, 6630, 1, 1, 0, 3,
     '', 'custom_enchant_npc');
