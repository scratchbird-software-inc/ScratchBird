#!/bin/bash
# PostgreSQL Version Information Collector
# Outputs JSON with comprehensive version/build information

set -e

OUTPUT_FILE="${1:-/tmp/postgresql-version.json}"

# Get PostgreSQL version
VERSION_INFO=$(psql -U "${POSTGRES_USER}" -d "${POSTGRES_DB}" -t -c "SELECT version();" 2>/dev/null | tr -s ' ' | sed 's/^ *//')

# Parse version number
VERSION_NUM=$(echo "$VERSION_INFO" | grep -oE "[0-9]+\.[0-9]+(\.[0-9]+)?" | head -1)
MAJOR=$(echo "$VERSION_NUM" | cut -d. -f1)
MINOR=$(echo "$VERSION_NUM" | cut -d. -f2)
PATCH=$(echo "$VERSION_NUM" | cut -d. -f3)
[ -z "$PATCH" ] && PATCH="0"

# Get server encoding
ENCODING=$(psql -U "${POSTGRES_USER}" -d "${POSTGRES_DB}" -t -c "SHOW server_encoding;" 2>/dev/null | tr -d ' ')

# Get locale
LC_COLLATE=$(psql -U "${POSTGRES_USER}" -d "${POSTGRES_DB}" -t -c "SHOW lc_collate;" 2>/dev/null | tr -d ' ')
LC_CTYPE=$(psql -U "${POSTGRES_USER}" -d "${POSTGRES_DB}" -t -c "SHOW lc_ctype;" 2>/dev/null | tr -d ' ')

# Get block size
BLOCK_SIZE=$(psql -U "${POSTGRES_USER}" -d "${POSTGRES_DB}" -t -c "SHOW block_size;" 2>/dev/null | tr -d ' ')

# Get shared buffers
SHARED_BUFFERS=$(psql -U "${POSTGRES_USER}" -d "${POSTGRES_DB}" -t -c "SHOW shared_buffers;" 2>/dev/null | tr -d ' ')

# Get max connections
MAX_CONNECTIONS=$(psql -U "${POSTGRES_USER}" -d "${POSTGRES_DB}" -t -c "SHOW max_connections;" 2>/dev/null | tr -d ' ')

# Get system info
ARCH=$(uname -m)
OS=$(uname -s)
OS_VERSION=$(uname -r)

# Get timestamp
TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

cat > "$OUTPUT_FILE" <<EOF
{
  "engine": "PostgreSQL",
  "version": {
    "major": $MAJOR,
    "minor": $MINOR,
    "patch": "$PATCH",
    "full": "$VERSION_NUM",
    "build_string": "$VERSION_INFO"
  },
  "configuration": {
    "encoding": "$ENCODING",
    "lc_collate": "$LC_COLLATE",
    "lc_ctype": "$LC_CTYPE",
    "block_size": "$BLOCK_SIZE",
    "shared_buffers": "$SHARED_BUFFERS",
    "max_connections": $MAX_CONNECTIONS
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
