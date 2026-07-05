#!/bin/bash
# MySQL Docker Entrypoint

set -e

ensure_mysql_security() {
    local mysql_root_cmd=""

    if mysqladmin -u root --silent ping 2>/dev/null; then
        mysql_root_cmd="mysql -u root"
    elif mysqladmin -u root -p"${MYSQL_ROOT_PASSWORD}" --silent ping 2>/dev/null; then
        mysql_root_cmd="mysql -u root -p${MYSQL_ROOT_PASSWORD}"
    else
        echo "Unable to connect as MySQL root user for setup"
        return 1
    fi

    # Set root password and create benchmark user/database
    $mysql_root_cmd <<EOF
CREATE DATABASE IF NOT EXISTS ${MYSQL_DATABASE} CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER IF NOT EXISTS '${MYSQL_USER}'@'localhost' IDENTIFIED WITH mysql_native_password BY '${MYSQL_PASSWORD}';
CREATE USER IF NOT EXISTS '${MYSQL_USER}'@'%' IDENTIFIED WITH mysql_native_password BY '${MYSQL_PASSWORD}';
ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY '${MYSQL_ROOT_PASSWORD}';
CREATE USER IF NOT EXISTS 'root'@'%' IDENTIFIED WITH mysql_native_password BY '${MYSQL_ROOT_PASSWORD}';
ALTER USER 'root'@'%' IDENTIFIED WITH mysql_native_password BY '${MYSQL_ROOT_PASSWORD}';
GRANT ALL PRIVILEGES ON ${MYSQL_DATABASE}.* TO '${MYSQL_USER}'@'localhost';
GRANT ALL PRIVILEGES ON ${MYSQL_DATABASE}.* TO '${MYSQL_USER}'@'%';
GRANT ALL PRIVILEGES ON *.* TO 'root'@'%' WITH GRANT OPTION;
FLUSH PRIVILEGES;
EOF
}

# Initialize MySQL data directory if needed
if [ ! -d "/var/lib/mysql/mysql" ]; then
    echo "Initializing MySQL data directory..."
    mysqld --initialize-insecure --user=mysql
fi

# Start temporary instance to guarantee user and database availability.
echo "Starting MySQL for security bootstrap..."
mysqld --skip-networking --socket=/var/run/mysqld/mysqld.sock &
TMP_MYSQL_PID=$!

# Wait for MySQL to be ready
for i in {1..60}; do
    if mysqladmin -u root --silent ping 2>/dev/null || mysqladmin -u root -p"${MYSQL_ROOT_PASSWORD}" --silent ping 2>/dev/null; then
        break
    fi
    sleep 1
done

ensure_mysql_security

# Shutdown temporary MySQL
mysqladmin -u root -p"${MYSQL_ROOT_PASSWORD}" shutdown || \
    mysqladmin -u root shutdown || true

wait "$TMP_MYSQL_PID" || true

echo "MySQL setup complete"

# Ensure proper permissions
chown -R mysql:mysql /var/lib/mysql /var/run/mysqld

# If command is provided, execute it
if [ "$1" != "mysqld" ]; then
    exec "$@"
fi

# Start MySQL in foreground
echo "Starting MySQL ${MYSQL_VERSION}..."
exec mysqld --user=mysql
