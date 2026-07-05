#!/bin/bash
# DDL Tests Runner
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RESULTS_DIR="$PROJECT_ROOT/results/ddl-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RESULTS_DIR"

ENGINE="${1:-all}"
echo "Running DDL tests for: $ENGINE"
echo "Results: $RESULTS_DIR"

python3 - "$PROJECT_ROOT" "$RESULTS_DIR" "$ENGINE" <<'PY'
import json
import sys
from datetime import datetime
from pathlib import Path

project_root = Path(sys.argv[1])
results_dir = Path(sys.argv[2])
engine = sys.argv[3]
sys.path.insert(0, str(project_root / "ddl-tests" / "scenarios"))
from ddl_tests import get_all_tests

tests = get_all_tests()
payload = {
    "metadata": {
        "suite": "ddl",
        "engine": engine,
        "timestamp": datetime.now().isoformat(),
        "profile": "ddl_scenario_catalog",
    },
    "results": [
        {
            "name": test.name,
            "description": test.description,
            "category": test.category,
            "setup_sql": test.setup_sql,
            "ddl_sql": test.ddl_sql,
            "verification_sql": test.verification_sql,
            "expected_result": test.expected_result,
        }
        for test in tests
    ],
    "summary": {
        "total_tests": len(tests),
        "categories": sorted({test.category for test in tests}),
    },
}
out = results_dir / f"ddl-{engine}.json"
out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
print(f"DDL scenario catalog written to: {out}")
PY
ls -la "$RESULTS_DIR"
