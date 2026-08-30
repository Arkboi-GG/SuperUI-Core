#!/usr/bin/env python3
"""Patches DurabilityQuality.dbc for the Reforged/Relic item-quality tiers (GOA branch).

RepairDurability (Player.cpp) and the repair-cost handler (ItemHandler.cpp) look up
this DBC by id = (ItemQuality + 1) * 2. Inserting ITEM_QUALITY_REFORGED=5 between
Epic and Legendary shifted Legendary from quality 5->6 and Artifact from 6->7, which
moves their DBC lookup ids from 12->14 and 14->16. This DBC is runtime-extracted data
outside both git repos, so a fresh map/DBC/vmap extraction wipes this fix -- re-run
this script against the newly extracted DurabilityQuality.dbc if that ever happens.

Usage: python3 patch_durability_quality.py <path to DurabilityQuality.dbc>
"""
import struct
import sys

HEADER_FMT = "<4sIIII"  # magic, recordCount, fieldCount, recordSize, stringBlockSize
RECORD_FMT = "<If"      # id (uint32), quality_mod (float) -- DBCfmt.h "nf"


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <DurabilityQuality.dbc>")
    path = sys.argv[1]

    with open(path, "rb") as f:
        data = f.read()

    header_size = struct.calcsize(HEADER_FMT)
    magic, record_count, field_count, record_size, string_block_size = struct.unpack(
        HEADER_FMT, data[:header_size]
    )
    if magic != b"WDBC" or field_count != 2 or record_size != 8:
        sys.exit("unexpected DBC layout -- aborting rather than guessing")

    records_end = header_size + record_count * record_size
    records = dict(
        struct.unpack(RECORD_FMT, data[o : o + record_size])
        for o in range(header_size, records_end, record_size)
    )
    string_block = data[records_end:]

    # id 12 currently holds the pre-shift Legendary value; id 14 the pre-shift
    # Artifact value (0.0 -- the long-standing "artifact repairs cost nothing,
    # forced to 1 copper" quirk in Player.cpp/ItemHandler.cpp). Re-home them to
    # their new ids and add the two new tiers.
    old_legendary_mod = records[12]
    old_artifact_mod = records[14]

    records[12] = 2.75          # ITEM_QUALITY_REFORGED  -> id (5+1)*2
    records[14] = old_legendary_mod   # ITEM_QUALITY_LEGENDARY -> id (6+1)*2, preserved
    records[16] = old_artifact_mod    # ITEM_QUALITY_ARTIFACT  -> id (7+1)*2, preserved (quirk intact)
    records[18] = 3.5           # ITEM_QUALITY_RELIC      -> id (8+1)*2

    new_record_count = len(records)
    new_header = struct.pack(
        HEADER_FMT, magic, new_record_count, field_count, record_size, string_block_size
    )
    new_records = b"".join(
        struct.pack(RECORD_FMT, rid, mod) for rid, mod in sorted(records.items())
    )

    with open(path, "wb") as f:
        f.write(new_header + new_records + string_block)

    print(f"Wrote {new_record_count} records (was {record_count}) to {path}")


if __name__ == "__main__":
    main()
