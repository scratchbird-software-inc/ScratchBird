#!/usr/bin/env python3
"""Query Optimizer Tests"""
from dataclasses import dataclass
from typing import List

@dataclass
class OptimizerTest:
    name: str
    description: str
    sql: str
    check_plan: str

class OptimizerTests:
    @staticmethod
    def get_all_tests() -> List[OptimizerTest]:
        return [
            OptimizerTest("plan_stability", "Same query produces same plan",
                         "SELECT * FROM orders WHERE order_id = 1", "plan_hash"),
            OptimizerTest("index_usage", "Index is used for selective query",
                         "SELECT * FROM orders WHERE customer_id = 100", "index_scan"),
            OptimizerTest("join_order", "Optimal join order selected",
                         "SELECT * FROM orders o JOIN customers c ON o.customer_id = c.customer_id", "join_order"),
            OptimizerTest("subquery_flattening", "Subquery is flattened to join",
                         "SELECT * FROM orders WHERE customer_id IN (SELECT customer_id FROM customers WHERE country = 'US')", "flattened"),
        ]

def get_all_tests():
    return OptimizerTests.get_all_tests()

if __name__ == '__main__':
    print(f"Optimizer Tests: {len(get_all_tests())}")
