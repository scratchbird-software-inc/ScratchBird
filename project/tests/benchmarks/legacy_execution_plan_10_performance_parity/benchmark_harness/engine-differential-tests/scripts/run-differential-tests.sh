#!/bin/bash
#
# Engine Differential Tests Runner
#
# Runs tests that perform differently based on engine architecture.
# These tests reveal where each engine excels and where it struggles.
#
# Usage: ./run-differential-tests.sh [engine] [category]
#   engine: firebird, mysql, postgresql, or all
#   category: mysql, postgresql, firebird, or all
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RESULTS_DIR="$PROJECT_ROOT/results/differential-$(date +%Y%m%d-%H%M%S)"

mkdir -p "$RESULTS_DIR"

ENGINE="${1:-all}"
CATEGORY="${2:-all}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}Engine Differential Test Suite${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""
echo "These tests exploit architectural differences:"
echo "  • MySQL: Clustered PK, covering indexes, buffer pool"
echo "  • PostgreSQL: Parallel query, advanced indexes (GiST/GIN), hash joins"
echo "  • Firebird: MGA (readers never block), compact storage, versioning"
echo ""
echo "Engine: $ENGINE"
echo "Category: $CATEGORY"
echo "Results: $RESULTS_DIR"
echo ""

wait_for_db() {
    local engine=$1
    local host=$2
    local port=$3
    local attempt=1
    echo -n "Waiting for $engine..."
    while [ $attempt -le 30 ]; do
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

    echo -e "\n${YELLOW}Running differential tests for $engine...${NC}"

    case $engine in
        firebird)
            HOST="firebird"; PORT=3050; DB="benchmark.fdb"
            ;;
        mysql)
            HOST="mysql"; PORT=3306; DB="benchmark"
            ;;
        postgresql)
            HOST="postgresql"; PORT=5432; DB="benchmark"
            ;;
    esac

    wait_for_db $engine $HOST $PORT

    # Run Python differential test runner
    docker run --rm \
        --network scratchbird-benchmarks_benchmark-net \
        -v "$PROJECT_ROOT/engine-differential-tests:/engine-differential-tests" \
        -v "$RESULTS_DIR:/results" \
        sb-stress-tests \
        python3 /engine-differential-tests/runners/differential_test_runner.py \
            --engine=$engine \
            --host=$HOST \
            --port=$PORT \
            --database="$DB" \
            --user=benchmark \
            --password=benchmark \
            --category="$CATEGORY" \
            --output-dir=/results \
        2>&1 | tee "$RESULTS_DIR/differential-${engine}.log"

    echo -e "${GREEN}Differential tests complete for $engine${NC}"
}

if [ "$ENGINE" = "all" ]; then
    cd "$PROJECT_ROOT"
    docker-compose up -d firebird mysql postgresql

    echo ""
    echo -e "${BLUE}Phase 1: Testing MySQL-optimized scenarios${NC}"
    echo "These should be FASTEST on MySQL, slower on PostgreSQL/Firebird"
    run_engine_tests "mysql"
    run_engine_tests "postgresql"
    run_engine_tests "firebird"

    echo ""
    echo -e "${BLUE}Phase 2: Testing PostgreSQL-optimized scenarios${NC}"
    echo "These should be FASTEST on PostgreSQL, slower on MySQL/Firebird"

    echo ""
    echo -e "${BLUE}Phase 3: Testing Firebird-optimized scenarios${NC}"
    echo "These should be FASTEST on Firebird, slower on MySQL/PostgreSQL"

else
    cd "$PROJECT_ROOT"
    docker-compose up -d $ENGINE
    run_engine_tests "$ENGINE"
fi

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Differential tests complete!${NC}"
echo -e "${GREEN}Results: $RESULTS_DIR${NC}"
echo -e "${GREEN}========================================${NC}"

echo ""
echo "Expected Results Summary:"
echo ""
echo "MySQL should WIN on:"
echo "  • Clustered PK range scans (5-20x faster)"
echo "  • Covering index queries (2-5x faster)"
echo "  • Buffer pool hot data access (2-4x faster)"
echo "  • Sequential PK inserts (2-5x faster)"
echo ""
echo "PostgreSQL should WIN on:"
echo "  • Parallel table scans (4-8x faster)"
echo "  • Full-text search with GIN (50-100x faster)"
echo "  • Hash joins (10-100x faster)"
echo "  • HOT updates (3-10x faster)"
echo ""
echo "Firebird should WIN on:"
echo "  • Concurrent readers (10-100x under contention)"
echo "  • Read-heavy workloads with writes (5-10x)"
echo "  • Compact NULL storage (30-50% smaller)"
echo "  • Transaction rollback (100-1000x faster)"
