#!/bin/bash
# Firebird Docker Entrypoint

set -e

# Ensure proper permissions
chown -R firebird:firebird /firebird/data

# Initialize security database if it doesn't exist
if [ ! -f "$FIREBIRD_HOME/security5.fdb" ]; then
    echo "Initializing Firebird security database..."
    cp "$FIREBIRD_HOME/examples/udr/security5.fdb" "$FIREBIRD_HOME/security5.fdb" 2>/dev/null || true
    chown firebird:firebird "$FIREBIRD_HOME/security5.fdb" 2>/dev/null || true
fi

# Create benchmark user in SECURITY database before server starts.
create_benchmark_user() {
    echo "Creating benchmark user..."
    $FIREBIRD_HOME/bin/isql -user SYSDBA -password masterkey "$FIREBIRD_HOME/security5.fdb" <<'SQL'
CREATE OR ALTER USER benchmark PASSWORD 'benchmark';
SQL
}

create_benchmark_user

# Start Firebird in background
echo "Starting Firebird $FB_VERSION..."
$FIREBIRD_HOME/bin/firebird &

# Wait for Firebird to be ready
echo "Waiting for Firebird to be ready..."
for i in {1..30}; do
    if echo "SELECT 1 FROM rdb\$database;" | $FIREBIRD_HOME/bin/isql -user SYSDBA -password masterkey -quiet 2>/dev/null; then
        echo "Firebird is ready"
        break
    fi
    sleep 1
    if [ "$i" -eq 30 ]; then
        echo "Firebird failed to start within 30 seconds"
    fi
done

# Ensure benchmark database exists.
if [ ! -f "/firebird/data/${FIREBIRD_DATABASE}" ]; then
    echo "Creating benchmark database..."
    $FIREBIRD_HOME/bin/isql -user SYSDBA -password masterkey <<SQL
CREATE DATABASE '/firebird/data/${FIREBIRD_DATABASE}' USER 'benchmark' PASSWORD 'benchmark';
COMMIT;
SQL
    echo "Benchmark database created"
fi

# Verify benchmark user can connect, otherwise recreate database to repair permissions.
if ! echo "SELECT 1 FROM rdb\$database;" | $FIREBIRD_HOME/bin/isql -u benchmark -p benchmark "/firebird/data/${FIREBIRD_DATABASE}" -q 2>/dev/null; then
    echo "Benchmark database verification failed; recreating as benchmark user..."
    rm -f "/firebird/data/${FIREBIRD_DATABASE}"
    $FIREBIRD_HOME/bin/isql -user SYSDBA -password masterkey <<SQL
CREATE DATABASE '/firebird/data/${FIREBIRD_DATABASE}' USER 'benchmark' PASSWORD 'benchmark';
COMMIT;
SQL
fi

# If command is provided, execute it
if [ "$1" != "firebird" ]; then
    exec "$@"
fi

# Keep Firebird running in foreground
wait
