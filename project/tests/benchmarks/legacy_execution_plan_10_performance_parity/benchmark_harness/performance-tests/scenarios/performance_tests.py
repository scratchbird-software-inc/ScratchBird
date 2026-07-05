#!/usr/bin/env python3
"""Performance Characterization Tests"""
from dataclasses import dataclass
from typing import List

@dataclass
class PerformanceTest:
    name: str
    description: str
    operation: str
    iterations: int
    metric: str

class PerformanceTests:
    @staticmethod
    def get_all_tests() -> List[PerformanceTest]:
        return [
            PerformanceTest("pk_lookup_latency", "Primary key lookup latency",
                          "SELECT * FROM orders WHERE order_id = :id", 10000, "p50_p95_p99"),
            PerformanceTest("insert_throughput", "INSERT operations per second",
                          "INSERT INTO perf_test VALUES (:id, :data)", 50000, "ops_per_sec"),
            PerformanceTest("range_scan", "Range scan performance",
                          "SELECT * FROM orders WHERE order_date BETWEEN :d1 AND :d2", 1000, "rows_per_sec"),
            PerformanceTest("aggregation_speed", "Aggregation query speed",
                          "SELECT COUNT(*), SUM(total), AVG(total) FROM orders", 1000, "queries_per_sec"),
            PerformanceTest("connection_overhead", "Connection establishment time",
                          "CONNECT", 100, "ms_per_connection"),
            PerformanceTest("concurrent_mixed", "Mixed workload concurrent",
                          "MIXED_READ_WRITE", 10000, "tps"),
        ]

def get_all_tests():
    return PerformanceTests.get_all_tests()

if __name__ == '__main__':
    print(f"Performance Tests: {len(get_all_tests())}")
