#!/usr/bin/env python3
"""
Fault Tolerance & Recovery Tests

Tests for crash recovery, resource exhaustion, and failure handling.
"""

from dataclasses import dataclass
from typing import List

@dataclass
class FaultTest:
    name: str
    description: str
    fault_type: str  # crash, resource, network, corruption
    trigger: str
    expected_recovery: str

class FaultToleranceTests:
    """Fault tolerance test scenarios."""

    @staticmethod
    def get_all_tests() -> List[FaultTest]:
        return [
            # Crash recovery tests
            FaultTest("crash_mid_transaction", "Crash during uncommitted transaction",
                     "crash", "KILL -9 during UPDATE", "Rollback on restart"),

            FaultTest("crash_during_commit", "Crash during COMMIT",
                     "crash", "KILL during COMMIT flush", "Durability check"),

            FaultTest("crash_during_checkpoint", "Crash during checkpoint",
                     "crash", "KILL during CHECKPOINT", "Consistent recovery"),

            # Resource exhaustion tests
            FaultTest("oom_large_query", "Out of memory on large query",
                     "resource", "SELECT * FROM huge_table ORDER BY all_columns", "Graceful error"),

            FaultTest("disk_full", "Disk full during write",
                     "resource", "Fill disk, attempt INSERT", "Error returned"),

            FaultTest("max_connections", "Max connections reached",
                     "resource", "Open connections > max", "Connection rejected"),

            FaultTest("lock_timeout", "Lock wait timeout",
                     "resource", "Hold lock, wait for timeout", "Timeout error"),

            # Network fault tests
            FaultTest("network_partition", "Network partition",
                     "network", "Drop packets", "Connection error"),

            FaultTest("slow_client", "Slow client simulation",
                     "network", "Delay ACKs", "No deadlock"),

            # Data corruption tests
            FaultTest("checksum_failure", "Page checksum failure",
                     "corruption", "Corrupt page on disk", "Error on read"),
        ]

def get_all_tests():
    return FaultToleranceTests.get_all_tests()

if __name__ == '__main__':
    tests = get_all_tests()
    print(f"Fault Tolerance Tests: {len(tests)}")
    for t in tests:
        print(f"  - {t.name} ({t.fault_type})")
