#!/bin/bash
#
# Run Full Regression Test Suite
#
# This script orchestrates running the complete upstream regression tests
# against both original engines and ScratchBird, then generates comparison
# reports.
#
# Usage:
#   ./run-regression-suite.sh [engine] [target]
#
#   engine: firebird, mysql, postgresql, or all
#   target: original (baseline) or scratchbird (emulation)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/../results/regression-$(date +%Y%m%d-%H%M%S)"

mkdir -p "$RESULTS_DIR"

ENGINE="${1:-all}"
TARGET="${2:-original}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "ScratchBird Regression Test Suite"
echo "=========================================="
echo "Engine: $ENGINE"
echo "Target: $TARGET"
echo "Results: $RESULTS_DIR"
echo ""

# Function to run Firebird FBT tests
run_firebird_tests() {
    echo -e "${YELLOW}Running Firebird FBT tests...${NC}"

    if [ "$TARGET" = "original" ]; then
        docker-compose -f "$SCRIPT_DIR/../docker-compose.yml" run --rm \
            fbt-runner \
            --target=original \
            --suite=all \
            --output-dir=/results \
            2>&1 | tee "$RESULTS_DIR/fbt-$TARGET.log"
    else
        # Run against ScratchBird in Firebird mode
        docker-compose -f "$SCRIPT_DIR/../docker-compose.yml" run --rm \
            fbt-runner \
            --target=scratchbird \
            --mode=firebird \
            --suite=all \
            --output-dir=/results \
            2>&1 | tee "$RESULTS_DIR/fbt-$TARGET.log"
    fi

    # Copy results from container
    docker cp "sb-fbt-runner:/results/" "$RESULTS_DIR/fbt-$TARGET/" 2>/dev/null || true

    echo -e "${GREEN}Firebird FBT tests complete${NC}"
}

# Function to run MySQL tests
run_mysql_tests() {
    echo -e "${YELLOW}Running MySQL tests...${NC}"

    if [ "$TARGET" = "original" ]; then
        docker-compose -f "$SCRIPT_DIR/../docker-compose.yml" run --rm \
            mysql-test-runner \
            --target=original \
            --suite=all \
            --output-dir=/results \
            2>&1 | tee "$RESULTS_DIR/mysql-$TARGET.log"
    else
        docker-compose -f "$SCRIPT_DIR/../docker-compose.yml" run --rm \
            mysql-test-runner \
            --target=scratchbird \
            --mode=mysql \
            --suite=all \
            --output-dir=/results \
            2>&1 | tee "$RESULTS_DIR/mysql-$TARGET.log"
    fi

    docker cp "sb-mysql-test-runner:/results/" "$RESULTS_DIR/mysql-$TARGET/" 2>/dev/null || true

    echo -e "${GREEN}MySQL tests complete${NC}"
}

# Function to run PostgreSQL tests
run_postgresql_tests() {
    echo -e "${YELLOW}Running PostgreSQL regression tests...${NC}"

    if [ "$TARGET" = "original" ]; then
        docker-compose -f "$SCRIPT_DIR/../docker-compose.yml" run --rm \
            pg-regress-runner \
            --target=original \
            --schedule=parallel \
            --output-dir=/results \
            2>&1 | tee "$RESULTS_DIR/pg-$TARGET.log"
    else
        docker-compose -f "$SCRIPT_DIR/../docker-compose.yml" run --rm \
            pg-regress-runner \
            --target=scratchbird \
            --mode=postgresql \
            --schedule=parallel \
            --output-dir=/results \
            2>&1 | tee "$RESULTS_DIR/pg-$TARGET.log"
    fi

    docker cp "sb-pg-regress-runner:/results/" "$RESULTS_DIR/pg-$TARGET/" 2>/dev/null || true

    echo -e "${GREEN}PostgreSQL tests complete${NC}"
}

# Run tests based on engine selection
case "$ENGINE" in
    firebird)
        run_firebird_tests
        ;;
    mysql)
        run_mysql_tests
        ;;
    postgresql)
        run_postgresql_tests
        ;;
    all)
        run_firebird_tests
        run_mysql_tests
        run_postgresql_tests
        ;;
    *)
        echo -e "${RED}Unknown engine: $ENGINE${NC}"
        echo "Usage: $0 [firebird|mysql|postgresql|all] [original|scratchbird]"
        exit 1
        ;;
esac

echo ""
echo -e "${GREEN}=========================================="
echo "Regression tests complete!"
echo "Results: $RESULTS_DIR"
echo "==========================================${NC}"

# If running scratchbird, suggest comparison
if [ "$TARGET" = "scratchbird" ]; then
    echo ""
    echo "To compare with baseline (original) results, run:"
    echo "  python3 $SCRIPT_DIR/runners/compare_results.py \\"
    echo "    --original=$RESULTS_DIR/../original/*.json \\"
    echo "    --scratchbird=$RESULTS_DIR/*/*.json \\"
    echo "    --output=$RESULTS_DIR/comparison.html"
fi
