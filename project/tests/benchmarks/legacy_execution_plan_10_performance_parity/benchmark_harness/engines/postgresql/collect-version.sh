#!/bin/bash
# Health check and version collection script for PostgreSQL

set -e

# Check if PostgreSQL is running
if ! pgrep -x "postgres" > /dev/null; then
    echo "PostgreSQL process not running"
    exit 1
fi

PG_BIN_DIR="/usr/lib/postgresql/${PG_MAJOR}/bin"

# Wait for PostgreSQL to be ready
for i in {1..30}; do
    if su - postgres -c "$PG_BIN_DIR/pg_isready -q" 2>/dev/null; then
        break
    fi
    sleep 1
done

# Get version info
VERSION=$(su - postgres -c "$PG_BIN_DIR/psql -t -c 'SELECT version();'" 2>/dev/null | head -1 | awk '{print $2}')

if [ -z "$VERSION" ]; then
    echo "Failed to get PostgreSQL version"
    exit 1
fi

# Write version info to benchmark results
mkdir -p /benchmark-results
cat > /benchmark-results/postgresql-version.json <<EOF
{
    "engine": "postgresql",
    "version": "$VERSION",
    "timestamp": "$(date -Iseconds)",
    "database": "${PG_DATABASE}",
    "status": "healthy"
}
EOF

echo "PostgreSQL $VERSION - Healthy"
exit 0
