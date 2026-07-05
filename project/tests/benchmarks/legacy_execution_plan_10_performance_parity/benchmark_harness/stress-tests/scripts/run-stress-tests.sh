#!/bin/bash
#
# Run Complete Stress Test Suite
#
# This script orchestrates stress tests across all engines:
# 1. Data generation and loading (millions of rows)
# 2. JOIN stress tests (all types, multi-table)
# 3. Bulk operation tests (INSERT, UPDATE, DELETE)
# 4. Verification and comparison
#
# Usage:
#   ./run-stress-tests.sh [engine] [scale]
#
#   engine: firebird, mysql, postgresql, or all
#   scale: small, medium, large, huge
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RESULTS_DIR="$PROJECT_ROOT/results/stress-$(date +%Y%m%d-%H%M%S)"

mkdir -p "$RESULTS_DIR"

ENGINE="${1:-all}"
SCALE="${2:-medium}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=========================================="
echo "ScratchBird Stress Test Suite"
echo "==========================================${NC}"
echo "Engine: $ENGINE"
echo "Scale: $SCALE"
echo "Results: $RESULTS_DIR"
echo ""

# Function to wait for database to be ready
wait_for_db() {
    local engine=$1
    local host=$2
    local port=$3
    local max_attempts=30
    local attempt=1

    echo -n "Waiting for $engine to be ready..."
    while [ $attempt -le $max_attempts ]; do
        if timeout 2 bash -c "</dev/tcp/$host/$port" 2>/dev/null; then
            echo " OK"
            return 0
        fi
        echo -n "."
        sleep 2
        attempt=$((attempt + 1))
    done

    echo " TIMEOUT"
    return 1
}

# Function to run stress tests for an engine
run_engine_tests() {
    local engine=$1
    local container_name="sb-benchmark-$engine"

    echo -e "\n${YELLOW}Running stress tests for $engine...${NC}"

    # Determine connection parameters
    case $engine in
        firebird)
            HOST="firebird"
            PORT=3050
            DATABASE="benchmark.fdb"
            USER="benchmark"
            PASS="benchmark"
            ;;
        mysql)
            HOST="mysql"
            PORT=3306
            DATABASE="benchmark"
            USER="benchmark"
            PASS="benchmark"
            ;;
        postgresql)
            HOST="postgresql"
            PORT=5432
            DATABASE="benchmark"
            USER="benchmark"
            PASS="benchmark"
            ;;
    esac

    # Wait for database
    wait_for_db $engine $HOST $PORT

    # Build and run stress test container
    echo "Building stress test container..."
    docker build -t sb-stress-tests "$PROJECT_ROOT/stress-tests" >/dev/null 2>&1

    echo "Running stress tests (this may take a while)..."
    docker run --rm \
        --network scratchbird-benchmarks_benchmark-net \
        -v "$RESULTS_DIR:/results" \
        sb-stress-tests \
        python3 runners/stress_test_runner.py \
            --engine=$engine \
            --host=$HOST \
            --port=$PORT \
            --database="$DATABASE" \
            --user="$USER" \
            --password="$PASS" \
            --scale=$SCALE \
            --output-dir=/results \
        2>&1 | tee "$RESULTS_DIR/stress-${engine}-${SCALE}.log"

    echo -e "${GREEN}Stress tests complete for $engine${NC}"
}

# Start engines if needed
if [ "$ENGINE" = "all" ]; then
    echo "Starting all database engines..."
    cd "$PROJECT_ROOT"
    docker-compose up -d firebird mysql postgresql

    # Run tests for each engine
    run_engine_tests "firebird"
    run_engine_tests "mysql"
    run_engine_tests "postgresql"

    # Generate comparison
    echo -e "\n${YELLOW}Generating comparison report...${NC}"
    python3 "$SCRIPT_DIR/compare-stress-results.py" \
        --results-dir="$RESULTS_DIR" \
        --output="$RESULTS_DIR/comparison.html"

else
    # Start specific engine
    echo "Starting $ENGINE..."
    cd "$PROJECT_ROOT"
    docker-compose up -d $ENGINE

    # Run tests
    run_engine_tests "$ENGINE"
fi

echo ""
echo -e "${GREEN}=========================================="
echo "Stress tests complete!"
echo "Results: $RESULTS_DIR"
echo "==========================================${NC}"

# Print summary
if [ -f "$RESULTS_DIR/comparison.html" ]; then
    echo ""
    echo "View comparison report at:"
    echo "  $RESULTS_DIR/comparison.html"
fi

echo ""
echo "Result files:"
ls -la "$RESULTS_DIR"/*.json 2>/dev/null || echo "  No JSON results found"
