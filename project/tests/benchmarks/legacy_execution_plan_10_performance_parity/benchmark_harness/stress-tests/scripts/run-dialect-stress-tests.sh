#!/bin/bash
#
# Run Dialect-Aware Stress Tests
#
# Executes stress tests using engine-native SQL dialects:
# - FirebirdSQL uses Firebird dialect 3
# - MySQL uses MySQL 8.0+ syntax
# - PostgreSQL uses PostgreSQL syntax
#
# Usage:
#   ./run-dialect-stress-tests.sh [engine] [scale]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RESULTS_DIR="$PROJECT_ROOT/results/stress-dialect-$(date +%Y%m%d-%H%M%S)"

mkdir -p "$RESULTS_DIR"

ENGINE="${1:-all}"
SCALE="${2:-medium}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}=========================================="
echo "Dialect-Aware Stress Test Suite"
echo "==========================================${NC}"
echo "Engine: $ENGINE"
echo "Scale: $SCALE"
echo "Results: $RESULTS_DIR"
echo ""
echo "Each engine will be tested using its NATIVE SQL dialect"
echo ""

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

run_engine_tests() {
    local engine=$1
    local container_name="sb-benchmark-$engine"

    echo -e "\n${YELLOW}Running dialect-aware stress tests for $engine...${NC}"

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

    wait_for_db $engine $HOST $PORT

    echo "Building stress test container..."
    docker build -t sb-stress-tests "$PROJECT_ROOT/stress-tests" >/dev/null 2>&1

    echo "Running stress tests with $engine dialect (this may take a while)..."
    docker run --rm \
        --network scratchbird-benchmarks_benchmark-net \
        -v "$RESULTS_DIR:/results" \
        sb-stress-tests \
        python3 runners/dialect_stress_runner.py \
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

if [ "$ENGINE" = "all" ]; then
    echo "Starting all database engines..."
    cd "$PROJECT_ROOT"
    docker-compose up -d firebird mysql postgresql

    run_engine_tests "firebird"
    run_engine_tests "mysql"
    run_engine_tests "postgresql"

    echo -e "\n${YELLOW}Generating comparison report...${NC}"
    python3 "$SCRIPT_DIR/compare-stress-results.py" \
        --results-dir="$RESULTS_DIR" \
        --output="$RESULTS_DIR/comparison.html"

else
    echo "Starting $ENGINE..."
    cd "$PROJECT_ROOT"
    docker-compose up -d $ENGINE

    run_engine_tests "$ENGINE"
fi

echo ""
echo -e "${GREEN}=========================================="
echo "Dialect-aware stress tests complete!"
echo "Results: $RESULTS_DIR"
echo "==========================================${NC}"

if [ -f "$RESULTS_DIR/comparison.html" ]; then
    echo ""
    echo "View comparison report at:"
    echo "  $RESULTS_DIR/comparison.html"
fi

echo ""
echo "Result files:"
ls -la "$RESULTS_DIR"/*.json 2>/dev/null || echo "  No JSON results found"
