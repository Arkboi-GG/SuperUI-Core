# SuperUI-Core — Installation

SuperUI-Core is a fork of [VMaNGOS](https://github.com/vmangos/core). **Building the
server and the general database/client setup are identical to upstream VMaNGOS** —
follow the upstream instructions for compiling, extracting client data (maps/vmaps/mmaps/DBC),
and loading the world database:

- Upstream build & database guide: <https://github.com/vmangos/core>

This document covers **only what SuperUI-Core adds on top of VMaNGOS** — the schema
deltas its C++ queries expect. These are easy to miss because the base VMaNGOS DBs
don't contain them, and skipping them makes **mangosd terminate during world load**.

> **Tested on Linux + MariaDB 10.x.** Commands below assume the default `mangos`/`mangos`
> DB credentials and database names `characters` / `mangos` (world) / `realmd` / `logs`;
> adjust to your setup.

---

## 1. Load the stock VMaNGOS databases first

Create and populate `realmd` (logon), `characters`, `logs`, and the world database
per the upstream guide. In this repo the base schema files are:

- `sql/logon.sql` → `realmd`
- `sql/characters.sql` → `characters`
- `sql/logs.sql` → `logs`
- world database → upstream full world dump (not shipped in this repo)

Apply upstream migrations as usual (the `sql/migrations/` mechanism records applied
IDs in each DB's `migrations` table, so re-running is safe).

---

## 2. Apply the SuperUI-Core schema deltas

### 2a. `playerbot` — required, or mangosd won't finish loading

SuperUI-Core's `PlayerBotMgr` selects 12 columns from `characters.playerbot`:

```
SELECT char_guid, chance, ai, race, class, level, map,
       position_x, position_y, position_z, name FROM playerbot
```

Historically the bundled `characters` schema created only `(char_guid, chance, comment, ai)`.
The missing columns make the query fail and mangosd exits at **`[PlayerBotMgr] Loading Bots ...`**.

- **Fresh install:** nothing to do — `sql/characters.sql` in this repo already defines the
  full 12-column table.
- **Existing database:** apply the migration (idempotent; safe even if the columns were
  already added at runtime by the MangosSuperUI web app):

  ```bash
  mysql -u mangos -pmangos characters < sql/migrations/20260820120000_characters.sql
  ```

### 2b. `vmangos_admin.lootifier_generated_items` — required, or mangosd won't finish loading

SuperUI-Core's Lootifier makes the **core** read this table while loading quest reward variants:

```
Loading quest reward variants...
SQL: SELECT base_entry, generated_entry, budget_pct
     FROM `vmangos_admin`.`lootifier_generated_items` WHERE creature_entry = 0
[1146] Table 'vmangos_admin.lootifier_generated_items' doesn't exist
Your database structure is not up to date...
```

It lives in the `vmangos_admin` database (shared with MangosSuperUI). Create it **before
first boot** — the web app that would otherwise create it may not have run yet:

```bash
sudo mysql -e "CREATE DATABASE IF NOT EXISTS vmangos_admin; GRANT ALL PRIVILEGES ON vmangos_admin.* TO 'mangos'@'localhost'; FLUSH PRIVILEGES;"
mysql -u mangos -pmangos < sql/superui/03_vmangos_admin_lootifier.sql
```

### 2c. Optional SuperUI feature tables

The RTS/World-State, Spell Creator, and related tables are normally created at runtime
by MangosSuperUI or the in-game RTS world-creation flow. To pre-create them (e.g. running
the core without the web app), apply:

```bash
mysql -u mangos -pmangos characters < sql/superui/01_characters_superui_rts.sql
mysql -u mangos -pmangos mangos     < sql/superui/02_world_superui.sql
```

See [`sql/superui/README.md`](sql/superui/README.md) for the table-by-table breakdown.

---

## 3. Verify before starting mangosd

```bash
# playerbot should report 12 columns
mysql -u mangos -pmangos -e "SELECT COUNT(*) AS playerbot_cols FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='characters' AND TABLE_NAME='playerbot';"
# lootifier table should exist (no 1146 error)
mysql -u mangos -pmangos -e "SELECT 'ok' FROM vmangos_admin.lootifier_generated_items LIMIT 0;"
```

If both pass, mangosd will load the world instead of terminating.

---

## 4. The MangosSuperUI web admin

The web interface, dashboards, editors, and bot control live in a separate project.
Install it **after** the core is up and logging in:

- MangosSuperUI: <https://github.com/Yafrovon/MangosSuperUI> (see its `INSTALL.md`)

---

## Troubleshooting

**mangosd terminates at "[PlayerBotMgr] Loading Bots ..."** — the `playerbot` columns are
missing; apply §2a.

**mangosd exits with "[1146] Table 'vmangos_admin.lootifier_generated_items' doesn't exist"
/ "Your database structure is not up to date"** — apply §2b before boot.
