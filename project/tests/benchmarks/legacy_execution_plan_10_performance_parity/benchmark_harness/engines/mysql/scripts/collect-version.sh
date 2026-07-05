#!/bin/bash
# MySQL Version Information Collector
# Outputs JSON with comprehensive version/build information

set -e

OUTPUT_FILE="${1:-/tmp/mysql-version.json}"

# Wait for MySQL to be ready
until mysql -u root -p"${MYSQL_ROOT_PASSWORD}" -e "SELECT 1;" > /dev/null 2>&1; do
    sleep 1
done

# Get version information
VERSION_INFO=$(mysql -u root -p"${MYSQL_ROOT_PASSWORD}" -N -e "SELECT VERSION();" 2>/dev/null | tr -d ' ')
VERSION_COMMENT=$(mysql -u root -p"${MYSQL_ROOT_PASSWORD}" -N -e "SELECT @@version_comment;" 2>/dev/null)

# Parse version components
MAJOR=$(echo "$VERSION_INFO" | cut -d. -f1)
MINOR=$(echo "$VERSION_INFO" | cut -d. -f2)
PATCH=$(echo "$VERSION_INFO" | cut -d. -f3 | cut -d- -f1)

# Get InnoDB version
INNODB_VERSION=$(mysql -u root -p"${MYSQL_ROOT_PASSWORD}" -N -e "SELECT @@innodb_version;" 2>/dev/null | tr -d ' ')

# Get SQL mode
SQL_MODE=$(mysql -u root -p"${MYSQL_ROOT_PASSWORD}" -N -e "SELECT @@sql_mode;" 2>/dev/null)

# Get character set
CHARSET=$(mysql -u root -p"${MYSQL_ROOT_PASSWORD}" -N -e "SELECT @@character_set_server;" 2>/dev/null | tr -d ' ')

# Get collation
COLLATION=$(mysql -u root -p"${MYSQL_ROOT_PASSWORD}" -N -e "SELECT @@collation_server;" 2>/dev/null | tr -d ' ')

# Get system info
ARCH=$(uname -m)
OS=$(uname -s)
OS_VERSION=$(uname -r)

# Get timestamp
TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

cat > "$OUTPUT_FILE" <<EOF
{
  "engine": "MySQL",
  "version": {
    "major": $MAJOR,
    "minor": $MINOR,
    "patch": "$PATCH",
    "full": "$VERSION_INFO",
    "comment": "$VERSION_COMMENT"
  },
  "storage_engine": {
    "name": "InnoDB",
    "version": "$INNODB_VERSION"
  },
  "configuration": {
    "sql_mode": "$SQL_MODE",
    "character_set": "$CHARSET",
    "collation": "$COLLATION"
  },
  "system": {
    "architecture": "$ARCH",
    "os": "$OS",
    "os_version": "$OS_VERSION",
    "container": true
  },
  "collected_at": "$TIMESTAMP",
  "benchmark_suite_version": "1.0.0"
}
EOF

cat "$OUTPUT_FILE"
