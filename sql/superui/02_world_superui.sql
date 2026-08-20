-- ============================================================================
-- SuperUI-Core :: mangos (world) additions
-- ============================================================================
-- Run against the `mangos` (world) database.
-- ----------------------------------------------------------------------------

-- custom_spell_meta — Spell Creator sidecar metadata.
-- Verbatim from MangosSuperUI/Services/SpellServices/SpellConfigService.cs:46-59.
-- Created lazily by the web app; shipped here for parity. Not read by the core
-- at boot, so it is not boot-critical, but harmless to pre-create.
CREATE TABLE IF NOT EXISTS `custom_spell_meta` (
  `entry`         INT NOT NULL PRIMARY KEY,
  `source_entry`  INT NOT NULL DEFAULT 0,
  `spell_name`    VARCHAR(255) NOT NULL DEFAULT '',
  `name_subtext`  VARCHAR(255) DEFAULT NULL,
  `description`   TEXT DEFAULT NULL,
  `tooltip`       TEXT DEFAULT NULL,
  `color_preset`  VARCHAR(32) DEFAULT NULL,
  `phase_params`  TEXT DEFAULT NULL,
  `icon_source`   VARCHAR(32) DEFAULT NULL,
  `icon_path`     VARCHAR(512) DEFAULT NULL,
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- RTS spell-preservation tables.
-- Verbatim from MangosSuperUI/Services/RtsHeroSpellWorldStore.cs:29-32.
-- superui_rts_spell_original clones the stock spell_template shape; the web app
-- populates it via INSERT..SELECT during RTS world creation.
CREATE TABLE IF NOT EXISTS `superui_rts_spell_original` LIKE `spell_template`;

CREATE TABLE IF NOT EXISTS `superui_rts_spell_original_state` (
  `id` TINYINT UNSIGNED NOT NULL PRIMARY KEY,
  `captured_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- custom_texts — the SuperUI Spell tools WRITE to this table (custom broadcast /
-- script text), and the core reads it via DB script texts, but nothing in the
-- app CREATEs it: it is assumed to already exist in the world DB. If your
-- VMaNGOS base already ships it, "IF NOT EXISTS" makes this a no-op. Structure
-- mirrors the live table (tools/port/data/live_schemas.json).
CREATE TABLE IF NOT EXISTS `custom_texts` (
  `entry`           mediumint(8) NOT NULL,
  `content_default` text NOT NULL,
  `content_loc1`    text,
  `content_loc2`    text,
  `content_loc3`    text,
  `content_loc4`    text,
  `content_loc5`    text,
  `content_loc6`    text,
  `content_loc7`    text,
  `content_loc8`    text,
  `sound`           mediumint(8) unsigned NOT NULL DEFAULT '0',
  `type`            tinyint(3) unsigned NOT NULL DEFAULT '0',
  `language`        tinyint(3) unsigned NOT NULL DEFAULT '0',
  `emote`           smallint(5) unsigned NOT NULL DEFAULT '0',
  `comment`         text,
  PRIMARY KEY (`entry`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8;
