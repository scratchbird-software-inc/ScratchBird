#!/bin/bash
# Health check and version collection script for MySQL

set -e

# Wait for MySQL benchmark user and TCP listener to be ready.
for i in {1..30}; do
    if mysqladmin --protocol=TCP -h127.0.0.1 -P3306 -u"${MYSQL_USER}" -p"${MYSQL_PASSWORD}" ping --silent 2>/dev/null; then
        break
    fi
    sleep 1
done

# Get version info
VERSION=$(mysql --protocol=TCP -h127.0.0.1 -P3306 -u"${MYSQL_USER}" -p"${MYSQL_PASSWORD}" -D "${MYSQL_DATABASE}" -e "SELECT VERSION();" --silent --skip-column-names 2>/dev/null)

if [ -z "$VERSION" ]; then
    echo "Failed to get MySQL version"
    exit 1
fi

# Write version info to benchmark results
mkdir -p /benchmark-results
cat > /benchmark-results/mysql-version.json <<EOF
{
    "engine": "mysql",
    "version": "$VERSION",
    "timestamp": "$(date -Iseconds)",
    "database": "${MYSQL_DATABASE}",
    "status": "healthy"
}
EOF

echo "MySQL $VERSION - Healthy"
exit 0
