#!/bin/bash
#
# Start Database Engines for ScratchBird Benchmarks
#
# This script builds and starts Firebird, MySQL, and PostgreSQL containers
# ready for running benchmark tests.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

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
ScratchBird Benchmark Engine Startup (ALL ENGINES)

WARNING: This script starts ALL THREE engines simultaneously.
For isolated benchmarks, use: ./start-engine.sh <engine> start

Usage: $0 [COMMAND] [OPTIONS]

Commands:
  build       Build all engine Docker images
  start       Start ALL engines (for comparison testing)
  stop        Stop all engines
  restart     Restart all engines
  status      Check engine status
  logs        Show engine logs
  clean       Stop and remove containers (keeps images)
  purge       Stop, remove containers AND delete images
  connect     Show connection commands for each engine

Options:
  -h, --help  Show this help message

Examples:
  $0 build              # Build all engine images
  $0 start              # Start ALL engines (uses more resources)
  $0 status             # Check if engines are running
  $0 logs firebird      # Show Firebird logs
  $0 connect            # Show how to connect to each engine

For Single-Engine Benchmarks (Recommended):
  ./start-engine.sh firebird start    # Start Firebird only
  ./start-engine.sh mysql start       # Start MySQL only
  ./start-engine.sh postgresql start  # Start PostgreSQL only

Then run benchmarks:
  ./run-benchmark.sh firebird all     # Run all tests on Firebird

EOF
}

check_docker() {
    if ! command -v docker &> /dev/null; then
        log_error "Docker is not installed"
        echo "Install Docker: https://docs.docker.com/get-docker/"
        exit 1
    fi

    if ! docker info &> /dev/null; then
        log_error "Cannot connect to Docker daemon"
        echo "Either:"
        echo "  1. Run: sudo usermod -aG docker \$USER && newgrp docker"
        echo "  2. Use sudo with this script: sudo $0"
        exit 1
    fi

    log_success "Docker is available"
}

build_engines() {
    log_section "Building Engine Images"

    cd "$PROJECT_DIR"

    log_info "Building Firebird 5.0.1..."
    if docker build -t sb-benchmark-firebird:latest engines/firebird/; then
        log_success "Firebird image built"
    else
        log_error "Firebird build failed"
        return 1
    fi

    log_info "Building MySQL 9.0.1..."
    if docker build -t sb-benchmark-mysql:latest engines/mysql/; then
        log_success "MySQL image built"
    else
        log_error "MySQL build failed"
        return 1
    fi

    log_info "Building PostgreSQL 16..."
    if docker build -t sb-benchmark-postgresql:latest engines/postgresql/; then
        log_success "PostgreSQL image built"
    else
        log_error "PostgreSQL build failed"
        return 1
    fi

    log_section "All Images Built Successfully"
    docker images | grep sb-benchmark
}

start_engines() {
    log_section "Starting Database Engines"

    # Create results directory
    mkdir -p "$PROJECT_DIR/results"

    # Check if images exist
    if ! docker images | grep -q sb-benchmark-firebird; then
        log_warn "Firebird image not found, building..."
        docker build -t sb-benchmark-firebird:latest "$PROJECT_DIR/engines/firebird/"
    fi

    if ! docker images | grep -q sb-benchmark-mysql; then
        log_warn "MySQL image not found, building..."
        docker build -t sb-benchmark-mysql:latest "$PROJECT_DIR/engines/mysql/"
    fi

    if ! docker images | grep -q sb-benchmark-postgresql; then
        log_warn "PostgreSQL image not found, building..."
        docker build -t sb-benchmark-postgresql:latest "$PROJECT_DIR/engines/postgresql/"
    fi

    # Create network if it doesn't exist
    if ! docker network ls | grep -q benchmark-net; then
        log_info "Creating benchmark network..."
        docker network create benchmark-net
    fi

    # Start Firebird
    log_info "Starting Firebird..."
    if docker ps | grep -q sb-benchmark-firebird; then
        log_warn "Firebird is already running"
    else
        docker rm -f sb-benchmark-firebird 2>/dev/null || true
        docker run -d \
            --name sb-benchmark-firebird \
            --network benchmark-net \
            -p 3050:3050 \
            -e FIREBIRD_DATABASE=benchmark.fdb \
            -e FIREBIRD_USER=benchmark \
            -e FIREBIRD_PASSWORD=benchmark \
            -v "$PROJECT_DIR/results:/benchmark-results" \
            sb-benchmark-firebird:latest
        log_success "Firebird started on port 3050"
    fi

    # Start MySQL
    log_info "Starting MySQL..."
    if docker ps | grep -q sb-benchmark-mysql; then
        log_warn "MySQL is already running"
    else
        docker rm -f sb-benchmark-mysql 2>/dev/null || true
        docker run -d \
            --name sb-benchmark-mysql \
            --network benchmark-net \
            -p 3306:3306 \
            -e MYSQL_ROOT_PASSWORD=rootpassword \
            -e MYSQL_DATABASE=benchmark \
            -e MYSQL_USER=benchmark \
            -e MYSQL_PASSWORD=benchmark \
            -v "$PROJECT_DIR/results:/benchmark-results" \
            sb-benchmark-mysql:latest
        log_success "MySQL started on port 3306"
    fi

    # Start PostgreSQL
    log_info "Starting PostgreSQL..."
    if docker ps | grep -q sb-benchmark-postgresql; then
        log_warn "PostgreSQL is already running"
    else
        docker rm -f sb-benchmark-postgresql 2>/dev/null || true
        docker run -d \
            --name sb-benchmark-postgresql \
            --network benchmark-net \
            -p 5432:5432 \
            -e POSTGRES_USER=benchmark \
            -e POSTGRES_PASSWORD=benchmark \
            -e POSTGRES_DB=benchmark \
            -v "$PROJECT_DIR/results:/benchmark-results" \
            sb-benchmark-postgresql:latest
        log_success "PostgreSQL started on port 5432"
    fi

    log_section "Engines Starting Up"
    log_info "Waiting for health checks (this may take 30-60 seconds)..."

    # Wait for engines to be healthy
    for i in {1..60}; do
        sleep 2

        firebird_healthy=false
        mysql_healthy=false
        postgres_healthy=false

        if docker exec sb-benchmark-firebird /usr/local/bin/collect-version.sh &> /dev/null; then
            firebird_healthy=true
        fi

        if docker exec sb-benchmark-mysql /usr/local/bin/collect-version.sh &> /dev/null; then
            mysql_healthy=true
        fi

        if docker exec sb-benchmark-postgresql /usr/local/bin/collect-version.sh &> /dev/null; then
            postgres_healthy=true
        fi

        if [ "$firebird_healthy" = true ] && [ "$mysql_healthy" = true ] && [ "$postgres_healthy" = true ]; then
            log_section "All Engines Ready!"
            show_status
            show_connect
            return 0
        fi

        if [ $((i % 10)) -eq 0 ]; then
            echo "  Still waiting... (Firebird: $firebird_healthy, MySQL: $mysql_healthy, PostgreSQL: $postgres_healthy)"
        fi
    done

    log_warn "Engines started but health checks incomplete"
    show_status
}

stop_engines() {
    log_section "Stopping Database Engines"

    for container in sb-benchmark-firebird sb-benchmark-mysql sb-benchmark-postgresql; do
        if docker ps | grep -q "$container"; then
            log_info "Stopping $container..."
            docker stop "$container" &> /dev/null || true
            log_success "$container stopped"
        fi
    done

    log_success "All engines stopped"
}

clean_engines() {
    stop_engines

    log_section "Removing Containers"

    for container in sb-benchmark-firebird sb-benchmark-mysql sb-benchmark-postgresql; do
        if docker ps -a | grep -q "$container"; then
            log_info "Removing $container..."
            docker rm "$container" &> /dev/null || true
        fi
    done

    log_success "All containers removed"
}

purge_engines() {
    clean_engines

    log_section "Removing Images"

    for image in sb-benchmark-firebird sb-benchmark-mysql sb-benchmark-postgresql; do
        if docker images | grep -q "$image"; then
            log_info "Removing image $image..."
            docker rmi "$image:latest" &> /dev/null || true
        fi
    done

    log_success "All images removed"
}

show_status() {
    log_section "Engine Status"

    echo -e "${CYAN}Container Status:${NC}"
    docker ps --filter "name=sb-benchmark-" --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"

    echo ""
    echo -e "${CYAN}Version Information:${NC}"

    if [ -f "$PROJECT_DIR/results/firebird-version.json" ]; then
        echo -n "  Firebird: "
        cat "$PROJECT_DIR/results/firebird-version.json" | grep '"version"' | cut -d'"' -f4 || echo "unknown"
    fi

    if [ -f "$PROJECT_DIR/results/mysql-version.json" ]; then
        echo -n "  MySQL: "
        cat "$PROJECT_DIR/results/mysql-version.json" | grep '"version"' | cut -d'"' -f4 || echo "unknown"
    fi

    if [ -f "$PROJECT_DIR/results/postgresql-version.json" ]; then
        echo -n "  PostgreSQL: "
        cat "$PROJECT_DIR/results/postgresql-version.json" | grep '"version"' | cut -d'"' -f4 || echo "unknown"
    fi
}

show_logs() {
    local engine="$1"

    if [ -z "$engine" ]; then
        log_error "Please specify an engine: firebird, mysql, or postgresql"
        return 1
    fi

    local container="sb-benchmark-$engine"

    if ! docker ps -a | grep -q "$container"; then
        log_error "Container $container not found"
        return 1
    fi

    log_info "Showing logs for $engine (Ctrl+C to exit)..."
    docker logs -f "$container"
}

show_connect() {
    log_section "Connection Information"

    cat << EOF
Firebird 5.0.1:
  Host:     localhost:3050
  Database: benchmark.fdb
  User:     benchmark
  Password: benchmark
  Example:  isql-fb -u benchmark -p benchmark localhost:benchmark.fdb

MySQL 9.0.1:
  Host:     localhost:3306
  Database: benchmark
  User:     benchmark
  Password: benchmark
  Root:     root / rootpassword
  Example:  mysql -u benchmark -pbenchmark -h 127.0.0.1 benchmark

PostgreSQL 16:
  Host:     localhost:5432
  Database: benchmark
  User:     benchmark
  Password: benchmark
  Example:  psql -U benchmark -h 127.0.0.1 -d benchmark

From inside containers:
  mysql -u benchmark -pbenchmark -h mysql benchmark
  psql -U benchmark -h postgresql -d benchmark

EOF
}

# Main
COMMAND="${1:-help}"

# Check Docker for all commands except help
case "$COMMAND" in
    build|start|stop|restart|status|logs|clean|purge)
        check_docker
        ;;
esac

case "$COMMAND" in
    build)
        build_engines
        ;;
    start)
        start_engines
        ;;
    stop)
        stop_engines
        ;;
    restart)
        stop_engines
        sleep 2
        start_engines
        ;;
    status)
        show_status
        ;;
    logs)
        show_logs "$2"
        ;;
    clean)
        clean_engines
        ;;
    purge)
        purge_engines
        ;;
    connect)
        show_connect
        ;;
    help|--help|-h|*)
        show_help
        ;;
esac
