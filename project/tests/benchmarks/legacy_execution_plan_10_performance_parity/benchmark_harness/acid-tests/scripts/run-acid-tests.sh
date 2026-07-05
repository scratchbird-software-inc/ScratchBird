#!/bin/bash
#
# Run ACID Compliance Tests
#
# Usage: ./run-acid-tests.sh [engine] [category]
#   engine: firebird, mysql, postgresql, or all
#   category: atomicity, consistency, isolation, durability (optional)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RESULTS_DIR="$PROJECT_ROOT/results/acid-$(date +%Y%m%d-%H%M%S)"

mkdir -p "$RESULTS_DIR"

ENGINE="${1:-all}"
CATEGORY="${2:-}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}=========================================="
echo "ACID Compliance Test Suite"
echo "==========================================${NC}"
echo "Engine: $ENGINE"
[ -n "$CATEGORY" ] && echo "Category: $CATEGORY"
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

    echo -e "\n${YELLOW}Running ACID tests for $engine...${NC}"

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

    CATEGORY_ARG=""
    [ -n "$CATEGORY" ] && CATEGORY_ARG="--category=$CATEGORY"

    docker build -t sb-acid-tests "$PROJECT_ROOT/acid-tests" >/dev/null 2>&1 || true

    docker run --rm \
        --network scratchbird-benchmarks_benchmark-net \
        -v "$PROJECT_ROOT/acid-tests:/acid-tests" \
        -v "$RESULTS_DIR:/results" \
        sb-stress-tests \
        python3 /acid-tests/runners/acid_test_runner.py \
            --engine=$engine \
            --host=$HOST \
            --port=$PORT \
            --database="$DB" \
            --user=benchmark \
            --password=benchmark \
            --output-dir=/results \
            $CATEGORY_ARG \
        2>&1 | tee "$RESULTS_DIR/acid-${engine}.log"

    echo -e "${GREEN}ACID tests complete for $engine${NC}"
}

if [ "$ENGINE" = "all" ]; then
    cd "$PROJECT_ROOT"
    docker-compose up -d firebird mysql postgresql

    run_engine_tests "firebird"
    run_engine_tests "mysql"
    run_engine_tests "postgresql"

    echo -e "\n${YELLOW}Generating comparison report...${NC}"
    python3 "$SCRIPT_DIR/../scripts/compare-acid-results.py" \
        --results-dir="$RESULTS_DIR" \
        --output="$RESULTS_DIR/comparison.html" 2>/dev/null || true
else
    cd "$PROJECT_ROOT"
    docker-compose up -d $ENGINE
    run_engine_tests "$ENGINE"
fi

echo ""
echo -e "${GREEN}=========================================="
echo "ACID tests complete!"
echo "Results: $RESULTS_DIR"
echo "==========================================${NC}"

# Print summary
echo ""
for f in "$RESULTS_DIR"/acid_*.json; do
    [ -f "$f" ] || continue
    engine=$(basename "$f" | cut -d_ -f2)
    passed=$(jq -r '.summary.passed' "$f")
    total=$(jq -r '.summary.total' "$f")
    score=$(jq -r '.summary.passed / .summary.total * 100' "$f")
    printf "%12s: %2d/%2d passed (%5.1f%%)\n" "$engine" "$passed" "$total" "$score"
done
