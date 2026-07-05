#!/bin/bash
# PostgreSQL Docker Entrypoint

set -e

PG_BIN_DIR="/usr/lib/postgresql/${PG_MAJOR}/bin"

# Initialize PostgreSQL data directory if needed
if [ ! -s "$PGDATA/PG_VERSION" ]; then
    echo "Initializing PostgreSQL data directory..."

    # Create data directory
    mkdir -p "$PGDATA"
    chown -R postgres:postgres "$PGDATA"
    chmod 700 "$PGDATA"

    # Initialize database cluster
    su - postgres -c "$PG_BIN_DIR/initdb -D $PGDATA --encoding=UTF8 --locale=en_US.UTF-8"

    # Copy configuration files
    cp /etc/postgresql/${PG_MAJOR}/main/postgresql.conf $PGDATA/
    cp /etc/postgresql/${PG_MAJOR}/main/pg_hba.conf $PGDATA/

    # Start PostgreSQL temporarily for setup
    echo "Starting PostgreSQL for initial setup..."
    su - postgres -c "$PG_BIN_DIR/pg_ctl -D $PGDATA start"

    # Wait for PostgreSQL to be ready
    for i in {1..30}; do
        if su - postgres -c "$PG_BIN_DIR/pg_isready -q" 2>/dev/null; then
            break
        fi
        sleep 1
    done

    # Create benchmark user and database
    echo "Creating benchmark user and database..."
    su - postgres -c "$PG_BIN_DIR/psql -v ON_ERROR_STOP=1" <<-EOSQL
        CREATE USER ${PG_USER} WITH PASSWORD '${PG_PASSWORD}' SUPERUSER;
        CREATE DATABASE ${PG_DATABASE} OWNER ${PG_USER};
        GRANT ALL PRIVILEGES ON DATABASE ${PG_DATABASE} TO ${PG_USER};
        \c ${PG_DATABASE}
        GRANT ALL ON SCHEMA public TO ${PG_USER};
EOSQL

    # Stop PostgreSQL (will be started in foreground)
    su - postgres -c "$PG_BIN_DIR/pg_ctl -D $PGDATA stop"

    echo "PostgreSQL initialization complete"
fi

# Ensure proper permissions
chown -R postgres:postgres "$PGDATA" /var/run/postgresql
chmod 700 "$PGDATA"

# If command is provided, execute it
if [ "$1" != "postgres" ]; then
    exec "$@"
fi

# Start PostgreSQL in foreground
echo "Starting PostgreSQL ${PG_VERSION}..."
exec su - postgres -c "$PG_BIN_DIR/postgres -D $PGDATA -c config_file=$PGDATA/postgresql.conf"
