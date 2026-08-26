# sql/superui — SuperUI-Core custom schema

Tables the SuperUI-Core systems (bots, RTS/World-State, Spell Creator, Lootifier)
add on top of the stock VMaNGOS databases. They are **not** part of the base
VMaNGOS schema, so a plain VMaNGOS database won't have them.

Most are created automatically at runtime by the **MangosSuperUI** web app or by
the in-game RTS world-creation flow. They are shipped here as well so that:

- an operator running **without** the web app (or before its first boot) can
  pre-create them, and
- the schema is versioned and auditable in this repo.

Every statement is idempotent (`CREATE TABLE IF NOT EXISTS`), safe to re-run.

> The **`playerbot`** column fix is **not** here — it belongs to the base
> `characters` schema and is applied via `sql/characters.sql` (fresh installs)
> and `sql/migrations/20260820120000_characters.sql` (existing installs). See
> [`INSTALL.md`](../../INSTALL.md).

## Files & target databases

| File | Target DB | Contents |
|------|-----------|----------|
| `01_characters_superui_rts.sql` | `characters` | RTS World-State rules & live state — `superui_worldstate`, `superui_rules_*`, `superui_faction`, `superui_heroes`, `superui_zone_control`, `superui_dungeon_control` |
| `02_world_superui.sql` | world | Spell Creator (`custom_spell_meta`), RTS spell preservation (`superui_rts_spell_original`, `..._state`), and `custom_texts` (no-op if your base already ships it) |
| `03_vmangos_admin_lootifier.sql` | `vmangos_admin` | Shared prerequisites: boot-critical `lootifier_generated_items`, plus MangosSuperUI's durable `bot_combat_loadout_queue` and dispatch indexes |

## Applying

```bash
mysql -u <user> -p characters      < sql/superui/01_characters_superui_rts.sql
mysql -u <user> -p <world-db-name> < sql/superui/02_world_superui.sql
mysql -u <user> -p                 < sql/superui/03_vmangos_admin_lootifier.sql   # creates the DB + required admin tables
```

Replace `<world-db-name>` with your world database (commonly `mangos`).

MangosSuperUI also ships its broader base admin schema at
`MangosSuperUI/sql/vmangos_admin_schema.sql`. Both definitions of
`bot_combat_loadout_queue` are intentionally identical. The web application's
startup migrations remain authoritative for upgrading an existing queue table.
