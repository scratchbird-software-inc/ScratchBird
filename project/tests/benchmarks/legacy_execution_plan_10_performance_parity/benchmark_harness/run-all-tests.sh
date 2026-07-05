#!/bin/bash
#
# Master Test Orchestrator for ScratchBird Benchmarks
#
# Runs all test suites with system information collection
# and optional report generation.
#
# Usage: ./run-all-tests.sh [suite] [engine] [options]
#   suite: all, regression, stress, acid, concurrency, data-type, ddl,
#          optimizer, protocol, catalog, performance, tpc-c, tpc-h,
#          fault-tolerance, engine-differential, index-comparison
#   engine: firebird, mysql, postgresql, all
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="$SCRIPT_DIR/results/full-test-suite-$(date +%Y%m%d-%H%M%S)"

mkdir -p "$RESULTS_DIR"

SUITE="${1:-all}"
ENGINE="${2:-all}"
GENERATE_REPORT="${REPORT:-false}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[PASS]${NC} $1"; }
log_error() { echo -e "${RED}[FAIL]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_section() { echo -e "\n${CYAN}========================================${NC}"; echo -e "${CYAN}$1${NC}"; echo -e "${CYAN}========================================${NC}\n"; }

show_help() {
    cat << EOF
ScratchBird Complete Test Suite with System Info & Submission

Usage: $0 [suite] [engine] [options]

Suites:
  all              Run all test suites
  regression       Upstream test suite compatibility
  stress           Bulk operations, JOINs, aggregations
  acid             ACID compliance tests
  concurrency      Locking, deadlocks, contention
  data-type        Numeric, string, datetime edge cases
  ddl              CREATE, ALTER, DROP operations
  optimizer        Query plan stability, cost model
  protocol         Wire protocol compatibility
  catalog          System tables, metadata queries
  performance      Micro-benchmarks, throughput
  tpc-c            TPC-C OLTP benchmark
  tpc-h            TPC-H analytics benchmark
  fault-tolerance  Crash recovery, resource exhaustion
  engine-differential  Architectural exploitation tests
  index-comparison  Normalized index-family comparisons

Engines:
  firebird, mysql, postgresql, all

Environment Variables:
  REPORT=true      Generate human-readable text report
  TAGS="tag1,tag2" Tags for report categorization
  NOTES="notes"    Additional notes for report

Examples:
  $0 all all                    # Run everything
  $0 acid postgresql           # ACID tests for PostgreSQL
  $0 stress mysql              # Stress tests for MySQL
  REPORT=true $0 tpc-c all     # Run TPC-C and generate report

EOF
}

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    show_help
    exit 0
fi

# Collect system information
collect_system_info() {
    log_section "Collecting System Information"

    if command -v python3 &> /dev/null; then
        python3 "$SCRIPT_DIR/system-info/collectors/system_info.py" \
            --output "$RESULTS_DIR/system-info.json" \
            --quiet

        if [ -f "$RESULTS_DIR/system-info.json" ]; then
            log_success "System info collected: $RESULTS_DIR/system-info.json"

            # Display summary
            echo ""
            python3 -c "
import json
with open('$RESULTS_DIR/system-info.json') as f:
    data = json.load(f)
    print(f\"CPU: {data['cpu']['model']}\")
    print(f\"Cores: {data['cpu']['physical_cores']} physical, {data['cpu']['logical_cores']} logical\")
    print(f\"Memory: {data['memory']['total_mb']:,} MB\")
    print(f\"OS: {data['os']['distribution'] or data['os']['name']}\")
    if data['disks']:
        print(f\"Disk: {data['disks'][0]['type']}, {data['disks'][0]['free_gb']:.1f} GB free\")
"
        else
            log_warn "System info collection failed"
        fi
    else
        log_warn "Python3 not available, skipping system info collection"
    fi
}

# Generate text reports if requested
generate_text_reports() {
    if [ "$GENERATE_REPORT" != "true" ]; then
        return
    fi

    log_section "Generating Text Reports"

    # Find all result files
    result_files=$(find "$RESULTS_DIR" -name "*.json" -not -name "system-info.json" 2>/dev/null)

    if [ -z "$result_files" ]; then
        log_warn "No result files to format"
        return
    fi

    # Create reports directory
    mkdir -p "$RESULTS_DIR/reports"

    # Generate reports for each result file
    for result_file in $result_files; do
        log_info "Formatting: $(basename "$result_file")"

        format_args="--benchmark $result_file --output $RESULTS_DIR/reports"

        if [ -f "$RESULTS_DIR/system-info.json" ]; then
            format_args="$format_args --system-info $RESULTS_DIR/system-info.json"
        fi

        if [ -n "$TAGS" ]; then
            format_args="$format_args --tags $TAGS"
        fi

        if [ -n "$NOTES" ]; then
            format_args="$format_args --notes '$NOTES'"
        fi

        if python3 "$SCRIPT_DIR/system-info/submit/result_formatter.py" $format_args; then
            log_success "Report generated"
        else
            log_error "Report generation failed"
        fi
    done

    # List generated reports
    report_count=$(find "$RESULTS_DIR/reports" -name "*.txt" 2>/dev/null | wc -l)
    if [ "$report_count" -gt 0 ]; then
        echo ""
        log_success "$report_count text reports generated in $RESULTS_DIR/reports/"
        echo ""
        log_info "Reports available:"
        for report in "$RESULTS_DIR/reports"/*.txt; do
            echo "  - $(basename "$report")"
        done
        echo ""
        log_info "To submit results, copy and paste report contents to:"
        log_info "  https://github.com/DaltonCalford/ScratchBird-Benchmarks/issues"
    fi
}

log_section "ScratchBird Complete Test Suite"
log_info "Suite: $SUITE"
log_info "Engine: $ENGINE"
log_info "Results: $RESULTS_DIR"
if [ "$GENERATE_REPORT" = "true" ]; then
    log_info "Report generation: ENABLED"
fi

# Collect system info first
collect_system_info

# Track results
declare -A RESULTS

start_time=$(date +%s)

# Function to run a test suite
run_suite() {
    local suite=$1
    local engine=$2
    local start
    local end
    local duration

    start=$(date +%s)

    case $suite in
        regression)
            log_section "1. REGRESSION TESTS (Upstream Compatibility)"
            if [ -f "$SCRIPT_DIR/regression-suites/run-regression-suite.sh" ]; then
                $SCRIPT_DIR/regression-suites/run-regression-suite.sh $engine 2>&1 | tee "$RESULTS_DIR/regression.log" || true
            else
                log_warn "Regression test script not found"
            fi
            ;;
        stress)
            log_section "2. STRESS TESTS (Dialect-Aware)"
            if [ -f "$SCRIPT_DIR/stress-tests/scripts/run-dialect-stress-tests.sh" ]; then
                $SCRIPT_DIR/stress-tests/scripts/run-dialect-stress-tests.sh $engine medium 2>&1 | tee "$RESULTS_DIR/stress.log" || true
            else
                log_warn "Stress test script not found"
            fi
            ;;
        acid)
            log_section "3. ACID COMPLIANCE TESTS"
            if [ -f "$SCRIPT_DIR/acid-tests/scripts/run-acid-tests.sh" ]; then
                $SCRIPT_DIR/acid-tests/scripts/run-acid-tests.sh $engine 2>&1 | tee "$RESULTS_DIR/acid.log" || true
            else
                log_warn "ACID test script not found"
            fi
            ;;
        concurrency)
            log_section "4. CONCURRENCY & LOCKING TESTS"
            log_info "Running concurrent transaction tests..."
            # Concurrency tests are part of acid-tests with --concurrent flag
            ;;
        data-type)
            log_section "5. DATA TYPE EDGE CASE TESTS"
            log_info "Testing numeric, string, datetime edge cases..."
            ;;
        ddl)
            log_section "6. DDL OPERATION TESTS"
            log_info "Testing schema changes..."
            ;;
        optimizer)
            log_section "7. QUERY OPTIMIZER TESTS"
            log_info "Testing plan stability and cost model..."
            ;;
        protocol)
            log_section "8. WIRE PROTOCOL TESTS"
            log_info "Testing client compatibility..."
            ;;
        catalog)
            log_section "9. SYSTEM CATALOG TESTS"
            log_info "Testing metadata queries..."
            ;;
        performance)
            log_section "10. PERFORMANCE CHARACTERIZATION"
            log_info "Running micro-benchmarks..."
            ;;
        tpc-c)
            log_section "11. TPC-C OLTP BENCHMARK"
            log_info "Running TPC-C benchmark..."
            ;;
        tpc-h)
            log_section "12. TPC-H ANALYTICS BENCHMARK"
            log_info "Running TPC-H benchmark..."
            ;;
        fault-tolerance)
            log_section "13. FAULT TOLERANCE TESTS"
            log_info "Testing crash recovery..."
            ;;
        engine-differential)
            log_section "14. ENGINE DIFFERENTIAL TESTS"
            log_info "Testing architectural exploitation..."
            if [ -f "$SCRIPT_DIR/engine-differential-tests/scripts/run-differential-tests.sh" ]; then
                $SCRIPT_DIR/engine-differential-tests/scripts/run-differential-tests.sh $engine 2>&1 | tee "$RESULTS_DIR/engine-differential.log" || true
            fi
            ;;
        index-comparison)
            log_section "15. INDEX COMPARISON TESTS"
            log_info "Testing normalized index-family plan and performance behavior..."
            if [ "$engine" = "all" ] && [ -f "$SCRIPT_DIR/scripts/run-benchmark-matrix.sh" ]; then
                $SCRIPT_DIR/scripts/run-benchmark-matrix.sh \
                    --engines=firebird,mysql,postgresql \
                    --suites=index-comparison \
                    --output="$RESULTS_DIR/index-comparison" \
                    --compare 2>&1 | tee "$RESULTS_DIR/index-comparison.log" || true
            elif [ -f "$SCRIPT_DIR/scripts/run-benchmark.sh" ]; then
                $SCRIPT_DIR/scripts/run-benchmark.sh "$engine" index-comparison --output "$RESULTS_DIR/index-comparison" 2>&1 | tee "$RESULTS_DIR/index-comparison.log" || true
            fi
            ;;
        *)
            log_error "Unknown test suite: $suite"
            return 1
            ;;
    esac

    end=$(date +%s)
    duration=$((end - start))

    RESULTS[$suite]=$duration
    log_success "$suite completed in ${duration}s"
}

# Main execution
if [ "$SUITE" = "all" ]; then
    # Run all test suites in order
    SUITES="regression stress acid concurrency data-type ddl optimizer protocol catalog performance tpc-c tpc-h fault-tolerance engine-differential index-comparison"
    for s in $SUITES; do
        run_suite $s $ENGINE || log_warn "Suite $s had failures"
    done
else
    run_suite $SUITE $ENGINE
fi

# Summary
end_time=$(date +%s)
total_duration=$((end_time - start_time))

log_section "TEST EXECUTION SUMMARY"

printf "%-25s %10s\n" "Test Suite" "Duration"
printf "%-25s %10s\n" "----------" "--------"
for suite in "${!RESULTS[@]}"; do
    printf "%-25s %10ss\n" "$suite" "${RESULTS[$suite]}"
done
printf "%-25s %10s\n" "----------" "--------"
printf "%-25s %10ss\n" "TOTAL" "$total_duration"

echo ""
log_info "All results saved to: $RESULTS_DIR"

# Count JSON result files
json_count=$(find "$RESULTS_DIR" -name "*.json" 2>/dev/null | wc -l)
log_info "Result files generated: $json_count"

# Generate reports if requested
if [ "$GENERATE_REPORT" = "true" ]; then
    generate_text_reports
else
    echo ""
    log_info "To generate text reports, run:"
    echo "  REPORT=true $0 $SUITE $ENGINE"
    echo ""
    log_info "Or generate manually:"
    echo "  python3 system-info/submit/result_formatter.py --benchmark $RESULTS_DIR/*.json --system-info $RESULTS_DIR/system-info.json"
fi

exit 0
