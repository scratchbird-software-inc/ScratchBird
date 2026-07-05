#!/bin/bash
# Health check and version collection script for Firebird

set -e

# Check if Firebird is running
if ! pgrep -x "firebird" > /dev/null && ! pgrep -x "fbserver" > /dev/null && ! pgrep -x "fb_smp_server" > /dev/null; then
    echo "Firebird process not running"
    exit 1
fi

# Set defaults if not provided
FIREBIRD_DATABASE="${FIREBIRD_DATABASE:-benchmark.fdb}"

# Try to connect and get version info
export ISC_USER=benchmark
export ISC_PASSWORD=benchmark

# Check if benchmark database exists, if not create it
if [ ! -f "/firebird/data/${FIREBIRD_DATABASE}" ]; then
    echo "Creating benchmark database..."
    $FIREBIRD_HOME/bin/isql -user SYSDBA -password masterkey <<EOF
CREATE DATABASE '/firebird/data/${FIREBIRD_DATABASE}' USER 'benchmark' PASSWORD 'benchmark';
COMMIT;
EOF
fi

# Get version - use single quotes for string literals (not double quotes)
VERSION_OUTPUT=$(echo "SELECT rdb\$get_context('SYSTEM', 'ENGINE_VERSION') FROM rdb\$database;" | \
    $FIREBIRD_HOME/bin/isql /firebird/data/${FIREBIRD_DATABASE} -q 2>/dev/null)

# Extract version number from output (format: "    RDB_GET_CONTEXT\n============\n5.0.1")
VERSION=$(echo "$VERSION_OUTPUT" | grep -E '^[0-9]+\.[0-9]+' | head -1 | tr -d '[:space:]')

# If that didn't work, try alternative method
if [ -z "$VERSION" ]; then
    # Try getting version from server binary
    if [ -f "$FIREBIRD_HOME/bin/fbserver" ]; then
        VERSION=$($FIREBIRD_HOME/bin/fbserver -z 2>&1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
    elif [ -f "$FIREBIRD_HOME/bin/fb_smp_server" ]; then
        VERSION=$($FIREBIRD_HOME/bin/fb_smp_server -z 2>&1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
    fi
fi

# Default version if we can't detect
if [ -z "$VERSION" ]; then
    VERSION="5.0.1"
fi

# Write version info to benchmark results
mkdir -p /benchmark-results
cat > /benchmark-results/firebird-version.json <<EOF
{
    "engine": "firebird",
    "version": "$VERSION",
    "timestamp": "$(date -Iseconds)",
    "database": "${FIREBIRD_DATABASE}",
    "status": "healthy"
}
EOF

echo "Firebird $VERSION - Healthy"
exit 0
