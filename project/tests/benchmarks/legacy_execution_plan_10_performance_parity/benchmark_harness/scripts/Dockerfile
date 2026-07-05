# Benchmark Runner Container
# Python-based test harness for orchestrating benchmarks

FROM python:3.11-slim

LABEL maintainer="ScratchBird Benchmarks"
LABEL description="Benchmark test harness runner"

# Install database drivers and tools
RUN apt-get update && apt-get install -y \
    gcc \
    libpq-dev \
    unixodbc-dev \
    curl \
    jq \
    sysstat \
    && rm -rf /var/lib/apt/lists/*

# Install Python dependencies
RUN pip install --no-cache-dir \
    psycopg2-binary \
    pymysql \
    pyodbc \
    sqlalchemy \
    requests \
    pandas \
    matplotlib \
    pyyaml \
    click \
    tabulate

# Install Firebird driver (fdb)
RUN pip install --no-cache-dir fdb

# Copy benchmark scripts
COPY scripts/ /scripts/
RUN chmod +x /scripts/*.sh

# Copy test suites
COPY test-suites/ /test-suites/

WORKDIR /scripts

ENTRYPOINT ["python3", "/scripts/benchmark_runner.py"]
