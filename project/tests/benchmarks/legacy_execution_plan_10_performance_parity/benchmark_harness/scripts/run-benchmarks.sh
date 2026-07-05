#!/bin/bash
# Main benchmark runner script
# Usage: ./run-benchmarks.sh [--engine=ENGINE] [--suite=SUITE] [--output=DIR]

set -e

# Parse arguments
ENGINE="all"
SUITE="all"
OUTPUT_DIR="./results"

while [[ $# -gt 0 ]]; do
    case $1 in
        --engine=*)
            ENGINE="${1#*=}"
            shift
            ;;
        --suite=*)
            SUITE="${1#*=}"
            shift
            ;;
        --output=*)
            OUTPUT_DIR="${1#*=}"
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--engine=ENGINE] [--suite=SUITE] [--output=DIR]"
            exit 1
            ;;
    esac
done

mkdir -p "$OUTPUT_DIR"

RUN_ID=$(date -u +"%Y%m%d-%H%M%S")
TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

echo "========================================"
echo "ScratchBird Benchmark Suite"
echo "========================================"
echo "Run ID: $RUN_ID"
echo "Timestamp: $TIMESTAMP"
echo "Engine: $ENGINE"
echo "Suite: $SUITE"
echo "Output: $OUTPUT_DIR"
echo ""

# Step 1: Collect version information
echo "Step 1: Collecting version information..."
./scripts/collect-all-versions.sh "$OUTPUT_DIR"
echo ""

# Step 2: Run benchmarks based on suite selection
if [ "$SUITE" = "all" ] || [ "$SUITE" = "micro" ]; then
    echo "Step 2a: Running micro-benchmarks..."
    python3 /scripts/benchmark_runner.py \
        --suite=micro \
        --engine="$ENGINE" \
        --output="$OUTPUT_DIR/micro-${RUN_ID}.json"
    echo ""
fi

if [ "$SUITE" = "all" ] || [ "$SUITE" = "concurrent" ]; then
    echo "Step 2b: Running concurrent benchmarks..."
    python3 /scripts/benchmark_runner.py \
        --suite=concurrent \
        --engine="$ENGINE" \
        --output="$OUTPUT_DIR/concurrent-${RUN_ID}.json"
    echo ""
fi

if [ "$SUITE" = "all" ] || [ "$SUITE" = "regression" ]; then
    echo "Step 2c: Running regression tests..."
    python3 /scripts/benchmark_runner.py \
        --suite=regression \
        --engine="$ENGINE" \
        --output="$OUTPUT_DIR/regression-${RUN_ID}.json"
    echo ""
fi

# Step 3: Generate summary report
echo "Step 3: Generating summary report..."
cat > "$OUTPUT_DIR/run-${RUN_ID}-summary.txt" << EOF
Benchmark Run Summary
=====================
Run ID: $RUN_ID
Timestamp: $TIMESTAMP
Engine: $ENGINE
Suite: $SUITE

Results Location: $OUTPUT_DIR/
- Versions: all-versions.json
- Micro Benchmarks: micro-${RUN_ID}.json
- Concurrent Benchmarks: concurrent-${RUN_ID}.json
- Regression Tests: regression-${RUN_ID}.json

View detailed results:
  cat $OUTPUT_DIR/version-summary.txt
EOF

echo ""
echo "========================================"
echo "Benchmark run complete!"
echo "Run ID: $RUN_ID"
echo "Results in: $OUTPUT_DIR/"
echo "========================================"
