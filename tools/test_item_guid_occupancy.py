"""Run actual C++ source-selection logic and its SQL on synthetic data only."""
from pathlib import Path
import re
import sqlite3
import subprocess
import tempfile
import unittest

ROOT = Path('/home/wowvmangos/vmangos')
SOURCE = (ROOT / 'src/game/ObjectGuid.cpp').read_text()
METHOD = SOURCE[SOURCE.index('void ObjectGuidGenerator<high>::LoadFromDB'):]
SELECTION = METHOD[METHOD.index('    std::string occupiedSource'):METHOD.index('    std::unique_ptr<QueryResult>')]
HEADER = (ROOT / 'src/game/ObjectGuid.h').read_text()
ITEM_HIGH = int(re.search(r'HIGHGUID_ITEM\s*=\s*(0x[0-9a-fA-F]+|\d+)', HEADER).group(1), 0)


class OccupancyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='item-guid-scope-')
        directory = Path(cls.temp.name)
        cpp = directory / 'selection.cpp'
        cls.exe = directory / 'selection'
        cpp.write_text('#include <string>\n#include <iostream>\n'
                       f'constexpr int HIGHGUID_ITEM = {ITEM_HIGH};\n'
                       'int main(int argc, char** argv) {\n'
                       'if(argc != 4) return 2;\n'
                       'int high = std::stoi(argv[1]);\n'
                       'char const* fieldName = argv[2];\n'
                       'char const* tableName = argv[3];\n' + SELECTION +
                       'std::cout << occupiedSource;\n}\n')
        subprocess.run(['g++', '-std=c++11', str(cpp), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.db = sqlite3.connect(':memory:')
        for table, column in [('item_instance', 'guid'), ('character_inventory', 'item_guid'),
                              ('auction', 'item_guid'), ('character_gifts', 'item_guid'),
                              ('item_loot', 'guid'), ('mail_items', 'item_guid'),
                              ('petition', 'charter_guid'), ('item_text', 'id'),
                              ('characters', 'guid'), ('corpse', 'guid')]:
            self.db.execute(f'CREATE TABLE {table} ({column} INTEGER PRIMARY KEY)')
        self.db.executemany('INSERT INTO item_instance VALUES (?)', [(1,), (4,), (2600,)])

    def tearDown(self):
        self.db.close()

    def selection(self, field='guid', table='item_instance', high=ITEM_HIGH):
        return subprocess.check_output([str(self.exe), str(high), field, table], text=True)

    def occupancy(self, field='guid', table='item_instance', high=ITEM_HIGH):
        source = self.selection(field, table, high)
        maximum = self.db.execute(f'SELECT MAX(`{field}`) FROM {source}').fetchone()[0]
        occupied = [r[0] for r in self.db.execute(
            f'SELECT `{field}` FROM {source} WHERE `{field}` BETWEEN 1 AND 3000 ORDER BY `{field}` ASC')]
        return maximum, occupied

    def test_foreign_inventory_orphan_is_reserved(self):
        self.db.execute('INSERT INTO character_inventory VALUES (2546)')
        self.assertNotIn(2546, [r[0] for r in self.db.execute('SELECT guid FROM item_instance')])
        self.assertIn(2546, self.occupancy()[1])

    def test_all_references_reserve_holes_and_high_water_mark(self):
        for table, value in [('character_inventory', 2546), ('auction', 2800),
                             ('character_gifts', 2), ('item_loot', 5), ('mail_items', 6), ('petition', 7)]:
            self.db.execute(f'INSERT INTO {table} VALUES (?)', (value,))
        maximum, occupied = self.occupancy()
        self.assertEqual(maximum, 2800)
        self.assertEqual(occupied, [1, 2, 4, 5, 6, 7, 2546, 2600, 2800])
        self.assertEqual([i for i in range(1, 10) if i not in occupied], [3, 8, 9])

    def test_duplicate_references_and_rows_preserved(self):
        self.db.execute('INSERT INTO character_inventory VALUES (4)')
        before = list(self.db.iterdump())
        self.assertEqual(self.occupancy()[1].count(4), 1)
        self.assertEqual(before, list(self.db.iterdump()))

    def test_same_high_item_text_allocator_uses_own_column_and_table(self):
        # This shared HIGHGUID_ITEM caller caused the v1 server startup failure.
        manager = (ROOT / 'src/game/ObjectMgr.cpp').read_text()
        header = (ROOT / 'src/game/ObjectMgr.h').read_text()
        self.assertIn('m_ItemTextIds.LoadFromDB("id", "item_text")', manager)
        self.assertRegex(header, r'ObjectSafeGuidGenerator<HIGHGUID_ITEM>\s+m_ItemTextIds')
        self.db.executemany('INSERT INTO item_text VALUES (?)', [(8,), (19,)])
        self.assertEqual(self.selection('id', 'item_text'), '`item_text`')
        self.assertEqual(self.occupancy('id', 'item_text'), (19, [8, 19]))

    def test_other_startup_allocators_remain_isolated(self):
        for table, high, value in [('characters', 0, 200), ('corpse', 0xF101, 900)]:
            with self.subTest(table=table):
                self.db.execute(f'INSERT INTO {table} VALUES (?)', (value,))
                self.assertEqual(self.selection('guid', table, high), f'`{table}`')
                self.assertEqual(self.occupancy('guid', table, high), (value, [value]))

    def test_both_queries_use_selected_source_without_mutating_it(self):
        method = METHOD[:METHOD.index('end:')]
        self.assertEqual(method.count('occupiedSource.c_str()'), 2)
        self.assertNotRegex(SELECTION, r'\b(?:DELETE|UPDATE|REPLACE|INSERT)\b')


if __name__ == '__main__':
    unittest.main(verbosity=2)

