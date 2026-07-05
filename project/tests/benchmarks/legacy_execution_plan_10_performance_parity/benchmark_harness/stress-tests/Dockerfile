# Stress Test Runner Container
# Supports Firebird, MySQL, and PostgreSQL

FROM python:3.11-slim

# Install database clients
RUN apt-get update && apt-get install -y \
    # Firebird client
    firebird3.0-utils \
    libfbclient2 \
    # MySQL client
    default-mysql-client \
    # PostgreSQL client
    postgresql-client \
    # General utilities
    curl \
    vim \
    && rm -rf /var/lib/apt/lists/*

# Install Python dependencies
RUN pip install --no-cache-dir \
    pymysql \
    psycopg2-binary \
    fdb \
    firebird-driver \
    protobuf \
    pandas \
    matplotlib \
    tabulate

# Copy stress test code
COPY generators/ /stress-tests/generators/
COPY scenarios/ /stress-tests/scenarios/
COPY runners/ /stress-tests/runners/
COPY scripts/ /stress-tests/scripts/

WORKDIR /stress-tests

# Default to help
CMD ["python3", "runners/stress_test_runner.py", "--help"]
