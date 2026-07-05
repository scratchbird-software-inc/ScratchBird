#!/bin/bash
#
# Start a Single Database Engine for ScratchBird Benchmarks
#
# This script starts ONE engine at a time to ensure benchmarks
# run in isolation without other engines consuming resources.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CURRENT_REPO_ROOT="$(cd "$PROJECT_DIR/../../../.." && pwd)"
SCRATCHBIRD_ROOT="${SCRATCHBIRD_ROOT:-$CURRENT_REPO_ROOT}"
SCRATCHBIRD_DRIVER_ROOT="${SCRATCHBIRD_DRIVER_ROOT:-$CURRENT_REPO_ROOT/project/drivers}"
SCRATCHBIRD_EXAMPLE_MANAGER="${SCRATCHBIRD_EXAMPLE_MANAGER:-$SCRATCHBIRD_ROOT/scripts/example_db_manager.sh}"

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

is_port_in_use() {
    local candidate="$1"
    ss -ltn | awk '{print $4}' | grep -Eq "(:|\\])${candidate}\$"
}

resolve_host_port() {
    local default_port="$1"
    local requested_port="${2:-}"
    local engine="${3:-engine}"
    local candidate
    local attempts=0
    local max_attempts=20

    if [ -n "$requested_port" ]; then
        candidate="$requested_port"
    else
        candidate="$default_port"
    fi

    candidate="${candidate//[^0-9]/}"
    if [ -z "$candidate" ]; then
        echo "$default_port"
        return
    fi

    while [ "$attempts" -lt "$max_attempts" ]; do
        if ! is_port_in_use "$candidate"; then
            echo "$candidate"
            return
        fi

        log_warn "$engine host port $candidate is occupied; trying next port" >&2
        candidate=$((candidate + 1))
        attempts=$((attempts + 1))
    done

    echo "$default_port"
}

show_help() {
    cat << EOF
Start a Single Database Engine for Benchmarks

Usage: $0 <ENGINE> [COMMAND]

Engines:
  firebird    FirebirdSQL 5.0.1 (port 3050)
  mysql       MySQL 8.4 (port 3306)
  postgresql  PostgreSQL 16 (port 5432)
  scratchbird ScratchBird Beta 1 native server (default port 17092)

Commands:
  start       Start the engine (default)
  stop        Stop the engine
  restart     Restart the engine
  status      Check engine status
  logs        Show engine logs
  build       Build the Docker image
  connect     Show connection info
  clean       Stop and remove container

Examples:
  $0 firebird start       # Start Firebird only
  $0 mysql status         # Check MySQL status
  $0 postgresql stop      # Stop PostgreSQL
  $0 firebird logs        # View Firebird logs
  $0 mysql connect        # Show MySQL connection info

Notes:
  - Only ONE engine should be running during benchmarks
  - Starting an engine will stop any other running engines
  - Use './run-benchmark.sh' to run tests against the active engine

EOF
}

check_docker() {
    if ! command -v docker &> /dev/null; then
        log_error "Docker is not installed"
        exit 1
    fi

    if ! docker info &> /dev/null; then
        log_error "Cannot connect to Docker daemon"
        echo "Run: sudo usermod -aG docker \$USER && newgrp docker"
        echo "Or use sudo with this script"
        exit 1
    fi
}

scratchbird_env_file() {
    echo "$PROJECT_DIR/.benchmark-engine-ports/scratchbird.env"
}

scratchbird_native_port() {
    echo "${BENCHMARK_SCRATCHBIRD_PORT:-17092}"
}

scratchbird_pg_port() {
    echo "${BENCHMARK_SCRATCHBIRD_PG_PORT:-17432}"
}

scratchbird_mysql_port() {
    echo "${BENCHMARK_SCRATCHBIRD_MYSQL_PORT:-17306}"
}

scratchbird_fb_port() {
    echo "${BENCHMARK_SCRATCHBIRD_FB_PORT:-17050}"
}

scratchbird_root_dir() {
    local env_file
    env_file="$(scratchbird_env_file)"
    if [ -f "$env_file" ]; then
        # shellcheck disable=SC1090
        . "$env_file"
    fi
    echo "${BENCHMARK_SCRATCHBIRD_ROOT:-${BENCHMARK_SCRATCHBIRD_ROOT_DIR:-/tmp/sb-benchmark-scratchbird}}"
}

scratchbird_pid_file() {
    local root_dir
    root_dir="$(scratchbird_root_dir)"
    echo "$root_dir/control/sb_server.pid"
}

scratchbird_is_running() {
    local pid_file
    pid_file="$(scratchbird_pid_file)"
    if [ ! -f "$pid_file" ]; then
        return 1
    fi

    local pid
    pid="$(cat "$pid_file" 2>/dev/null || true)"
    if [ -z "$pid" ]; then
        return 1
    fi

    kill -0 "$pid" 2>/dev/null
}

write_scratchbird_env_file() {
    local root_dir="$1"
    local runtime_env="$root_dir/profiles/runtime.env"
    local auth_env="$root_dir/profiles/auth_defaults.env"
    local port_file
    local host_value="${BENCHMARK_SCRATCHBIRD_HOST:-127.0.0.1}"
    local port_value
    local pg_port_value
    local mysql_port_value
    local fb_port_value
    local db_value="${BENCHMARK_SCRATCHBIRD_DB:-main}"
    local user_value="${BENCHMARK_SCRATCHBIRD_USER:-SysArch}"
    local password_value="${BENCHMARK_SCRATCHBIRD_PASSWORD:-replaceme}"
    port_file="$(scratchbird_env_file)"
    port_value="$(scratchbird_native_port)"
    pg_port_value="$(scratchbird_pg_port)"
    mysql_port_value="$(scratchbird_mysql_port)"
    fb_port_value="$(scratchbird_fb_port)"

    if [ -f "$runtime_env" ]; then
        # shellcheck disable=SC1090
        . "$runtime_env"
        host_value="${SCRATCHBIRD_NATIVE_HOST:-$host_value}"
        port_value="${SCRATCHBIRD_NATIVE_PORT:-$port_value}"
        db_value="${SCRATCHBIRD_NATIVE_DB:-$db_value}"
        user_value="${SCRATCHBIRD_NATIVE_USER:-$user_value}"
        password_value="${SCRATCHBIRD_NATIVE_PASSWORD:-$password_value}"
    fi

    if [ -f "$auth_env" ]; then
        # shellcheck disable=SC1090
        . "$auth_env"
        user_value="${ADMIN_USER:-$user_value}"
        password_value="${ADMIN_PASSWORD:-$password_value}"
    fi

    mkdir -p "$PROJECT_DIR/.benchmark-engine-ports"
    {
        echo "BENCHMARK_SCRATCHBIRD_ROOT=$root_dir"
        echo "BENCHMARK_SCRATCHBIRD_RUNTIME_ENV=$runtime_env"
        echo "BENCHMARK_SCRATCHBIRD_HOST=${host_value}"
        echo "BENCHMARK_SCRATCHBIRD_PORT=${port_value}"
        echo "BENCHMARK_SCRATCHBIRD_PG_PORT=${pg_port_value}"
        echo "BENCHMARK_SCRATCHBIRD_MYSQL_PORT=${mysql_port_value}"
        echo "BENCHMARK_SCRATCHBIRD_FB_PORT=${fb_port_value}"
        echo "BENCHMARK_SCRATCHBIRD_DB=${db_value}"
        echo "BENCHMARK_SCRATCHBIRD_USER=${user_value}"
        echo "BENCHMARK_SCRATCHBIRD_PASSWORD=${password_value}"
    } > "$port_file"
}

start_scratchbird_engine() {
    local root_dir="${BENCHMARK_SCRATCHBIRD_ROOT:-/tmp/sb-benchmark-scratchbird}"
    local sb_isql_candidate

    if [ ! -x "$SCRATCHBIRD_EXAMPLE_MANAGER" ]; then
        log_error "ScratchBird example manager not found: $SCRATCHBIRD_EXAMPLE_MANAGER"
        return 1
    fi

    stop_all_engines
    rm -rf "$root_dir"
    export SCRATCHBIRD_EXAMPLE_DYNAMIC_ROOT="$root_dir"
    export SCRATCHBIRD_EXAMPLE_IMPORT_BUNDLE="${SCRATCHBIRD_EXAMPLE_IMPORT_BUNDLE:-0}"
    export SCRATCHBIRD_EXAMPLE_DYNAMIC_NATIVE_PORT="$(scratchbird_native_port)"
    export SCRATCHBIRD_EXAMPLE_DYNAMIC_PG_PORT="$(scratchbird_pg_port)"
    export SCRATCHBIRD_EXAMPLE_DYNAMIC_MYSQL_PORT="$(scratchbird_mysql_port)"
    export SCRATCHBIRD_EXAMPLE_DYNAMIC_FB_PORT="$(scratchbird_fb_port)"
    export SCRATCHBIRD_EXAMPLE_PARSER_ENGINE_RESPONSE_TIMEOUT_MS="${SCRATCHBIRD_EXAMPLE_PARSER_ENGINE_RESPONSE_TIMEOUT_MS:-300000}"

    if [ -z "${SCRATCHBIRD_SB_ISQL:-}" ]; then
        for sb_isql_candidate in \
            "$CURRENT_REPO_ROOT/build/drivers/tool/cli/sb_isql" \
            "$CURRENT_REPO_ROOT/build/drivers/tool/cli/src/sb_isql" \
            "$CURRENT_REPO_ROOT/build/current_release_benchmark/project/drivers/tool/cli/sb_isql"
        do
            if [ -x "$sb_isql_candidate" ]; then
                export SCRATCHBIRD_SB_ISQL="$sb_isql_candidate"
                break
            fi
        done
    fi

    log_section "Starting scratchbird"
    if ! "$SCRATCHBIRD_EXAMPLE_MANAGER" dynamic-setup; then
        if ! scratchbird_is_running; then
            log_error "ScratchBird example manager failed to start a runnable runtime"
            return 1
        fi
        log_warn "ScratchBird example manager returned non-zero after bringing the runtime up; continuing with live runtime detection"
    fi
    write_scratchbird_env_file "$root_dir"
    log_success "ScratchBird started from $root_dir"
    show_engine_status "scratchbird"
    show_engine_connect "scratchbird"
}

stop_all_engines() {
    # Stop any other running benchmark engines to ensure isolation
    local container_name
    for container_name in sb-benchmark-firebird sb-benchmark-mysql sb-benchmark-postgresql; do
        if docker ps | grep -q "$container_name"; then
            log_info "Stopping $container_name for isolation..."
            docker stop "$container_name" &> /dev/null || true
        fi
    done
    if scratchbird_is_running; then
        log_info "Stopping scratchbird for isolation..."
        stop_engine "scratchbird"
    fi
}

stop_engine() {
    local engine="$1"
    local container="sb-benchmark-$engine"

    if [ "$engine" = "scratchbird" ]; then
        local root_dir
        root_dir="$(scratchbird_root_dir)"
        if [ -x "$SCRATCHBIRD_EXAMPLE_MANAGER" ]; then
            export SCRATCHBIRD_EXAMPLE_DYNAMIC_ROOT="$root_dir"
            "$SCRATCHBIRD_EXAMPLE_MANAGER" dynamic-teardown >/dev/null 2>&1 || true
        fi
        rm -f "$(scratchbird_env_file)"
        if scratchbird_is_running; then
            log_warn "ScratchBird teardown did not fully stop the server"
            return 1
        fi
        log_success "scratchbird stopped"
        return 0
    fi

    if docker ps | grep -q "$container"; then
        log_info "Stopping $engine..."
        docker stop "$container" &> /dev/null || true
        log_success "$engine stopped"
    else
        log_warn "$engine is not running"
    fi
}

write_engine_port_file() {
    local engine="$1"
    local port_value="$2"
    local port_file="$PROJECT_DIR/.benchmark-engine-ports/${engine}.env"

    mkdir -p "$PROJECT_DIR/.benchmark-engine-ports"
    printf 'BENCHMARK_%s_PORT=%s\n' "$(printf "%s" "$engine" | tr '[:lower:]' '[:upper:]')" "$port_value" > "$port_file"
}

build_engine() {
    local engine="$1"

    if [ "$engine" = "scratchbird" ]; then
        log_info "ScratchBird uses the sibling workspace build instead of a Docker image"
        return 0
    fi

    log_section "Building $engine Image"

    cd "$PROJECT_DIR"

    case "$engine" in
        firebird)
            docker build -t sb-benchmark-firebird:latest engines/firebird/
            ;;
        mysql)
            docker build -t sb-benchmark-mysql:latest engines/mysql/
            ;;
        postgresql)
            docker build -t sb-benchmark-postgresql:latest engines/postgresql/
            ;;
    esac

    log_success "$engine image built"
}

start_engine() {
    local engine="$1"
    local container="sb-benchmark-$engine"
    local results_dir="${BENCHMARK_RESULTS_DIR:-$PROJECT_DIR/results}"
    local port

    log_section "Starting $engine"

    # Create and expose a writable results directory for in-container health checks.
    mkdir -p "$results_dir"
    chmod a+rwX "$results_dir"
    rm -f \
        "$results_dir/firebird-version.json" \
        "$results_dir/mysql-version.json" \
        "$results_dir/postgresql-version.json"

    # Stop all other engines first (isolation)
    stop_all_engines

    if [ "$engine" = "scratchbird" ]; then
        start_scratchbird_engine
        return 0
    fi

    # Check if image exists
    if ! docker images | grep -q "sb-benchmark-$engine"; then
        log_warn "$engine image not found, building..."
        build_engine "$engine"
    fi

    # Create network if it doesn't exist
    if ! docker network ls | grep -q benchmark-net; then
        log_info "Creating benchmark network..."
        docker network create benchmark-net
    fi

    # Remove existing container if any
    docker rm -f "$container" 2>/dev/null || true

    # Start the specific engine
    case "$engine" in
        firebird)
            port=$(resolve_host_port "${BENCHMARK_FIREBIRD_PORT:-3050}" "$BENCHMARK_FIREBIRD_PORT" "firebird")
            write_engine_port_file "firebird" "$port"
            docker run -d \
                --name "$container" \
                --network benchmark-net \
                -p "$port:3050" \
                -e FIREBIRD_DATABASE=benchmark.fdb \
                -e FIREBIRD_USER=benchmark \
                -e FIREBIRD_PASSWORD=benchmark \
                -v "$results_dir:/benchmark-results" \
                --memory="2g" \
                --cpus="2" \
                sb-benchmark-firebird:latest
            log_success "Firebird started on host port $port (container 3050)"
            ;;
        mysql)
            port=$(resolve_host_port "${BENCHMARK_MYSQL_PORT:-3306}" "$BENCHMARK_MYSQL_PORT" "mysql")
            write_engine_port_file "mysql" "$port"
            docker run -d \
                --name "$container" \
                --network benchmark-net \
                -p "$port:3306" \
                -e MYSQL_ROOT_PASSWORD=rootpassword \
                -e MYSQL_DATABASE=benchmark \
                -e MYSQL_USER=benchmark \
                -e MYSQL_PASSWORD=benchmark \
                -v "$results_dir:/benchmark-results" \
                --memory="2g" \
                --cpus="2" \
                sb-benchmark-mysql:latest
            log_success "MySQL started on host port $port (container 3306)"
            ;;
        postgresql)
            port=$(resolve_host_port "${BENCHMARK_POSTGRESQL_PORT:-5432}" "$BENCHMARK_POSTGRESQL_PORT" "postgresql")
            write_engine_port_file "postgresql" "$port"
            docker run -d \
                --name "$container" \
                --network benchmark-net \
                -p "$port:5432" \
                -e POSTGRES_USER=benchmark \
                -e POSTGRES_PASSWORD=benchmark \
                -e POSTGRES_DB=benchmark \
                -v "$results_dir:/benchmark-results" \
                --memory="2g" \
                --cpus="2" \
                sb-benchmark-postgresql:latest
            log_success "PostgreSQL started on host port $port (container 5432)"
            ;;
    esac

    case "$engine" in
        firebird)
            export BENCHMARK_FIREBIRD_PORT="$port"
            ;;
        mysql)
            export BENCHMARK_MYSQL_PORT="$port"
            ;;
        postgresql)
            export BENCHMARK_POSTGRESQL_PORT="$port"
            ;;
    esac

    log_info "Waiting for $engine to be ready..."

    # Wait for health check
    for i in {1..60}; do
        sleep 2
        if docker exec "$container" /usr/local/bin/collect-version.sh &> /dev/null; then
            log_success "$engine is ready!"
            show_engine_status "$engine"
            show_engine_connect "$engine"
            return 0
        fi

        if [ $((i % 10)) -eq 0 ]; then
            echo "  Still waiting for $engine... ($i seconds)"
        fi
    done

    log_error "$engine failed to start within 2 minutes"
    docker logs "$container" --tail 50
    return 1
}

show_engine_status() {
    local engine="$1"
    local container="sb-benchmark-$engine"

    if [ "$engine" = "scratchbird" ]; then
        local root_dir
        local env_file
        root_dir="$(scratchbird_root_dir)"
        env_file="$(scratchbird_env_file)"
        echo ""
        echo -e "${CYAN}ScratchBird Runtime:${NC}"
        if scratchbird_is_running; then
            echo "scratchbird running from $root_dir"
        else
            echo "scratchbird stopped"
        fi
        if [ -f "$env_file" ]; then
            echo ""
            echo -e "${CYAN}Connection Variables:${NC}"
            cat "$env_file"
        fi
        return 0
    fi

    echo ""
    echo -e "${CYAN}Container Status:${NC}"
    docker ps --filter "name=$container" --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"

    echo ""
    echo -e "${CYAN}Version:${NC}"
    if [ -f "$PROJECT_DIR/results/${engine}-version.json" ]; then
        cat "$PROJECT_DIR/results/${engine}-version.json" | grep '"version"' | cut -d'"' -f4 || echo "unknown"
    fi
}

get_engine_host_port() {
    case "$1" in
        firebird) echo "${BENCHMARK_FIREBIRD_PORT:-3050}" ;;
        mysql) echo "${BENCHMARK_MYSQL_PORT:-3306}" ;;
        postgresql) echo "${BENCHMARK_POSTGRESQL_PORT:-5432}" ;;
        scratchbird)
            if [ -f "$(scratchbird_env_file)" ]; then
                # shellcheck disable=SC1090
                . "$(scratchbird_env_file)"
            fi
            echo "$(scratchbird_native_port)"
            ;;
    esac
}

show_engine_connect() {
    local engine="$1"

    log_section "Connection Information"

    case "$engine" in
        firebird)
            cat << EOF
Firebird 5.0.1:
  Host:     localhost:$(get_engine_host_port firebird)
  Database: /firebird/data/benchmark.fdb
  User:     benchmark
  Password: benchmark

  Command:  isql-fb -u benchmark -p benchmark localhost:/firebird/data/benchmark.fdb

  Environment for tests:
    export FB_HOST=localhost
    export FB_PORT=$(get_engine_host_port firebird)
    export FB_DATABASE=/firebird/data/benchmark.fdb
    export FB_USER=benchmark
    export FB_PASSWORD=benchmark
EOF
            ;;
        mysql)
            cat << EOF
MySQL 8.4:
  Host:     localhost:$(get_engine_host_port mysql)
  Database: benchmark
  User:     benchmark
  Password: benchmark

  Command:  mysql -u benchmark -pbenchmark -h 127.0.0.1 benchmark

  Environment for tests:
    export MYSQL_HOST=localhost
    export MYSQL_PORT=$(get_engine_host_port mysql)
    export MYSQL_DATABASE=benchmark
    export MYSQL_USER=benchmark
    export MYSQL_PASSWORD=benchmark
EOF
            ;;
        postgresql)
            cat << EOF
PostgreSQL 16:
  Host:     localhost:$(get_engine_host_port postgresql)
  Database: benchmark
  User:     benchmark
  Password: benchmark

  Command:  psql -U benchmark -h 127.0.0.1 -d benchmark

  Environment for tests:
    export PGHOST=localhost
    export PGPORT=$(get_engine_host_port postgresql)
    export PGDATABASE=benchmark
    export PGUSER=benchmark
    export PGPASSWORD=benchmark
EOF
            ;;
        scratchbird)
            if [ -f "$(scratchbird_env_file)" ]; then
                # shellcheck disable=SC1090
                . "$(scratchbird_env_file)"
            fi
            cat << EOF
ScratchBird Beta 1 Native:
  Host:     ${BENCHMARK_SCRATCHBIRD_HOST:-127.0.0.1}:$(get_engine_host_port scratchbird)
  Database: ${BENCHMARK_SCRATCHBIRD_DB:-main}
  User:     ${BENCHMARK_SCRATCHBIRD_USER:-bootstrap_admin}
  Password: ${BENCHMARK_SCRATCHBIRD_PASSWORD:-SbExampleBootstrap_2026!}

  Environment for tests:
    export SCRATCHBIRD_HOST=${BENCHMARK_SCRATCHBIRD_HOST:-127.0.0.1}
    export SCRATCHBIRD_PORT=$(get_engine_host_port scratchbird)
    export SCRATCHBIRD_DATABASE=${BENCHMARK_SCRATCHBIRD_DB:-main}
    export SCRATCHBIRD_USER=${BENCHMARK_SCRATCHBIRD_USER:-bootstrap_admin}
    export SCRATCHBIRD_PASSWORD=${BENCHMARK_SCRATCHBIRD_PASSWORD:-SbExampleBootstrap_2026!}
    export SCRATCHBIRD_ROOT=$(scratchbird_root_dir)
EOF
            ;;
    esac
}

show_logs() {
    local engine="$1"
    local container="sb-benchmark-$engine"

    if [ "$engine" = "scratchbird" ]; then
        local root_dir
        root_dir="$(scratchbird_root_dir)"
        local server_log="$root_dir/logs/sb_server.log"
        if [ ! -f "$server_log" ]; then
            log_error "ScratchBird server log not found: $server_log"
            return 1
        fi
        log_info "Showing logs for scratchbird (Ctrl+C to exit)..."
        tail -f "$server_log"
        return 0
    fi

    if ! docker ps -a | grep -q "$container"; then
        log_error "$engine container not found"
        return 1
    fi

    log_info "Showing logs for $engine (Ctrl+C to exit)..."
    docker logs -f "$container"
}

clean_engine() {
    local engine="$1"
    local container="sb-benchmark-$engine"

    stop_engine "$engine"

    if [ "$engine" = "scratchbird" ]; then
        return 0
    fi

    if docker ps -a | grep -q "$container"; then
        log_info "Removing $container..."
        docker rm "$container" &> /dev/null || true
        log_success "$container removed"
    fi
}

# Main
ENGINE="${1:-}"
COMMAND="${2:-start}"

if [ -z "$ENGINE" ] || [ "$ENGINE" == "--help" ] || [ "$ENGINE" == "-h" ]; then
    show_help
    exit 0
fi

# Validate engine
case "$ENGINE" in
    firebird|mysql|postgresql|scratchbird)
        ;;
    *)
        log_error "Unknown engine: $ENGINE"
        echo "Valid engines: firebird, mysql, postgresql, scratchbird"
        exit 1
        ;;
esac

# Validate and execute command
if [ "$ENGINE" != "scratchbird" ]; then
    check_docker
fi

case "$COMMAND" in
    start|up)
        start_engine "$ENGINE"
        ;;
    stop|down)
        stop_engine "$ENGINE"
        ;;
    restart)
        stop_engine "$ENGINE"
        sleep 2
        start_engine "$ENGINE"
        ;;
    status)
        show_engine_status "$ENGINE"
        ;;
    logs)
        show_logs "$ENGINE"
        ;;
    build)
        build_engine "$ENGINE"
        ;;
    connect)
        show_engine_connect "$ENGINE"
        ;;
    clean|remove|rm)
        clean_engine "$ENGINE"
        ;;
    *)
        log_error "Unknown command: $COMMAND"
        show_help
        exit 1
        ;;
esac
