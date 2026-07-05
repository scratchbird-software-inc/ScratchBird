#!/bin/bash
# Firebird Version Information Collector
# Outputs JSON with comprehensive version/build information

set -e

OUTPUT_FILE="${1:-/tmp/firebird-version.json}"

# Get Firebird server version
FB_VERSION=$(isql-fb -user sysdba -password masterkey -query "SELECT rdb$get_context('SYSTEM', 'ENGINE_VERSION') FROM rdb\$database;" 2>/dev/null | grep -v "============" | grep -v "RDB" | tr -d ' ')

# Get detailed version info
FB_VERSION_FULL=$(/usr/local/firebird/bin/fbserver -version 2>/dev/null || echo "Firebird 4.0.4")

# Get build information
FB_BUILD=$(strings /usr/local/firebird/bin/fbserver 2>/dev/null | grep -E "Firebird [0-9]" | head -1 || echo "4.0.4")

# Get ODS version
ODS_VERSION=$(isql-fb -user sysdba -password masterkey -query "SELECT mon\$ods_major || '.' || mon\$ods_minor FROM mon\$database;" 2>/dev/null | grep -v "============" | grep -v "MON" | tr -d ' ')

# Get page size
PAGE_SIZE=$(isql-fb -user sysdba -password masterkey -query "SELECT mon\$page_size FROM mon\$database;" 2>/dev/null | grep -v "============" | grep -v "MON" | tr -d ' ')

# Get architecture
ARCH=$(uname -m)
OS=$(uname -s)
OS_VERSION=$(uname -r)

# Get current timestamp
TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

# Create JSON output
cat > "$OUTPUT_FILE" <<EOF
{
  "engine": "FirebirdSQL",
  "version": {
    "major": 4,
    "minor": 0,
    "patch": 4,
    "full": "$FB_VERSION",
    "build_string": "$FB_BUILD"
  },
  "database": {
    "ods_version": "$ODS_VERSION",
    "page_size": $PAGE_SIZE,
    "dialect": 3
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
