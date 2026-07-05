#!/bin/bash
#
# Setup Python environment for ScratchBird Benchmarks
#
# Handles externally-managed-environment issues on Ubuntu/Debian
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[PASS]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[FAIL]${NC} $1"; }

show_help() {
    cat << EOF
Setup Python Environment for ScratchBird Benchmarks

Usage: $0 [METHOD]

Methods:
  venv      Create virtual environment (recommended)
  user      Install to user directory (~/.local)
  system    Install system-wide with --break-system-packages
  apt       Install only from apt repositories (limited)
  check     Check current Python dependencies

Examples:
  $0 venv       # Create and setup virtual environment
  $0 user       # Install to user directory
  $0 check      # Check what's already installed

EOF
}

check_deps() {
    log_info "Checking Python dependencies..."

    local missing=()

    # Check psutil
    if python3 -c "import psutil" 2>/dev/null; then
        log_success "psutil: installed"
    else
        missing+=("psutil")
        log_warn "psutil: NOT installed"
    fi

    # Check fdb
    if python3 -c "import fdb" 2>/dev/null; then
        log_success "fdb: installed"
    else
        missing+=("fdb")
        log_warn "fdb: NOT installed"
    fi

    # Check PyMySQL
    if python3 -c "import pymysql" 2>/dev/null; then
        log_success "pymysql: installed"
    else
        log_warn "pymysql: NOT installed (optional for MySQL)"
    fi

    # Check psycopg2
    if python3 -c "import psycopg2" 2>/dev/null; then
        log_success "psycopg2: installed"
    else
        log_warn "psycopg2: NOT installed (optional for PostgreSQL)"
    fi

    if [ ${#missing[@]} -eq 0 ]; then
        log_success "All required dependencies are installed!"
        return 0
    else
        log_warn "Missing dependencies: ${missing[*]}"
        return 1
    fi
}

setup_venv() {
    log_info "Setting up virtual environment..."

    cd "$PROJECT_DIR"

    # Create venv if it doesn't exist
    if [ ! -d ".venv" ]; then
        log_info "Creating virtual environment..."
        python3 -m venv .venv
    fi

    # Activate and install
    log_info "Installing packages in virtual environment..."
    source .venv/bin/activate
    pip install --upgrade pip
    pip install psutil fdb

    log_success "Virtual environment setup complete!"
    echo ""
    echo "To activate the virtual environment, run:"
    echo "  source $PROJECT_DIR/.venv/bin/activate"
    echo ""
    echo "Then run benchmarks:"
    echo "  sudo ./scripts/run-benchmark.sh firebird all --report"
}

setup_user() {
    log_info "Installing packages to user directory..."

    pip3 install --user psutil fdb

    log_success "Packages installed to user directory"
    log_info "Make sure ~/.local/bin is in your PATH"
}

setup_system() {
    log_warn "Installing system-wide with --break-system-packages"
    log_warn "This may conflict with system packages"
    read -p "Continue? [y/N]: " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi

    pip3 install --break-system-packages psutil fdb
    log_success "Packages installed system-wide"
}

setup_apt() {
    log_info "Installing from apt repositories..."

    # psutil is available in apt
    sudo apt update
    sudo apt install -y python3-psutil

    log_warn "fdb is not available in Ubuntu repositories"
    log_info "Please use one of the other methods to install fdb:"
    echo "  $0 venv    # Recommended"
    echo "  $0 user    # Install to user directory"
    echo "  $0 system  # Install system-wide"
}

# Main
METHOD="${1:-help}"

case "$METHOD" in
    venv)
        setup_venv
        ;;
    user)
        setup_user
        ;;
    system)
        setup_system
        ;;
    apt)
        setup_apt
        ;;
    check)
        check_deps
        ;;
    help|--help|-h|*)
        show_help
        ;;
esac
