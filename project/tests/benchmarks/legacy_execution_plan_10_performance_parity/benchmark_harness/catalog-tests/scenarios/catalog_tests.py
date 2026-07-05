#!/usr/bin/env python3
"""System Catalog & Metadata Tests"""
from dataclasses import dataclass
from typing import List

@dataclass
class CatalogTest:
    name: str
    description: str
    query: str
    expected_columns: List[str]

class CatalogTests:
    @staticmethod
    def get_all_tests() -> List[CatalogTest]:
        return [
            CatalogTest("list_tables", "List all tables",
                       "SELECT table_name FROM information_schema.tables WHERE table_schema = 'public'",
                       ["table_name"]),
            CatalogTest("list_columns", "List table columns",
                       "SELECT column_name, data_type FROM information_schema.columns WHERE table_name = 'orders'",
                       ["column_name", "data_type"]),
            CatalogTest("list_indexes", "List table indexes",
                       "SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'orders'",
                       ["indexname", "indexdef"]),
            CatalogTest("pk_info", "Primary key information",
                       "SELECT kcu.column_name FROM information_schema.table_constraints tc JOIN information_schema.key_column_usage kcu ON tc.constraint_name = kcu.constraint_name WHERE tc.constraint_type = 'PRIMARY KEY' AND tc.table_name = 'orders'",
                       ["column_name"]),
            CatalogTest("dbeaver_compat", "DBeaver metadata query",
                       "SELECT * FROM information_schema.tables", ["table_name", "table_type"]),
        ]

def get_all_tests():
    return CatalogTests.get_all_tests()

if __name__ == '__main__':
    print(f"Catalog Tests: {len(get_all_tests())}")
