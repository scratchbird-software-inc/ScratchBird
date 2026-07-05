#!/bin/bash
# Collect version information from all benchmark engines
# Outputs a unified JSON file with all engine versions

set -e

OUTPUT_DIR="${1:-./results}"
mkdir -p "$OUTPUT_DIR"

TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
DATE=$(date -u +"%Y-%m-%d")
RUN_ID=$(date -u +"%Y%m%d-%H%M%S")

echo "Collecting version information from all engines..."
echo "Run ID: $RUN_ID"
echo "Timestamp: $TIMESTAMP"
echo ""

# Collect from each engine
declare -A ENGINES
ENGINES=(
    ["firebird"]="3050"
    ["mysql"]="3306"
    ["postgresql"]="5432"
)

VERSION_DATA='{"run_id": "'$RUN_ID'", "timestamp": "'$TIMESTAMP'", "date": "'$DATE'", "engines": {}}'

for ENGINE in "${!ENGINES[@]}"; do
    PORT="${ENGINES[$ENGINE]}"
    echo "Collecting from $ENGINE (port $PORT)..."

    # Wait for engine to be ready
    MAX_RETRIES=30
    RETRY=0
    while ! docker exec "sb-benchmark-$ENGINE" /usr/local/bin/collect-version.sh "/tmp/version.json" 2>/dev/null; do
        RETRY=$((RETRY + 1))
        if [ $RETRY -ge $MAX_RETRIES ]; then
            echo "ERROR: Could not collect version from $ENGINE after $MAX_RETRIES retries"
            break
        fi
        echo "  Waiting for $ENGINE... ($RETRY/$MAX_RETRIES)"
        sleep 2
    done

    if [ $RETRY -lt $MAX_RETRIES ]; then
        # Copy version file from container
        docker cp "sb-benchmark-$ENGINE:/tmp/version.json" "$OUTPUT_DIR/${ENGINE}-version.json"
        echo "  ✓ Collected $ENGINE version info"
    else
        echo "  ✗ Failed to collect $ENGINE version info"
    fi
done

# Create unified version report
python3 << PYTHON_SCRIPT
import json
import sys
from pathlib import Path

output_dir = Path("$OUTPUT_DIR")
run_data = {
    "run_id": "$RUN_ID",
    "timestamp": "$TIMESTAMP",
    "date": "$DATE",
    "benchmark_suite_version": "1.0.0",
    "engines": {}
}

engines = ["firebird", "mysql", "postgresql"]
for engine in engines:
    version_file = output_dir / f"{engine}-version.json"
    if version_file.exists():
        try:
            with open(version_file) as f:
                run_data["engines"][engine] = json.load(f)
        except Exception as e:
            print(f"Warning: Could not parse {engine} version: {e}", file=sys.stderr)
            run_data["engines"][engine] = {"error": str(e)}
    else:
        run_data["engines"][engine] = {"error": "Version file not found"}

# Write unified report
output_file = output_dir / "all-versions.json"
with open(output_file, 'w') as f:
    json.dump(run_data, f, indent=2)

print(f"Unified version report written to: {output_file}")

# Also create a human-readable summary
summary_file = output_dir / "version-summary.txt"
with open(summary_file, 'w') as f:
    f.write(f"Benchmark Run: {run_data['run_id']}\n")
    f.write(f"Timestamp: {run_data['timestamp']}\n")
    f.write(f"Suite Version: {run_data['benchmark_suite_version']}\n")
    f.write("=" * 60 + "\n\n")

    for engine, data in run_data['engines'].items():
        f.write(f"{engine.upper()}:\n")
        if 'error' in data:
            f.write(f"  Error: {data['error']}\n")
        else:
            version = data.get('version', {})
            f.write(f"  Version: {version.get('full', 'unknown')}\n")
            f.write(f"  Build: {version.get('build_string', version.get('comment', 'unknown'))[:50]}\n")
        f.write("\n")

print(f"Summary written to: {summary_file}")
PYTHON_SCRIPT

echo ""
echo "Version collection complete."
echo "Results in: $OUTPUT_DIR/"
