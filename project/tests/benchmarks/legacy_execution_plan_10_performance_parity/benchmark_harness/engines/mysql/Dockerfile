# MySQL 8.4 for ScratchBird Benchmarks
# Uses the official image and a benchmark-specific config/init layer.

FROM mysql:8.4

LABEL maintainer="ScratchBird Benchmarks"
LABEL description="MySQL 8.4 benchmark engine"

ENV MYSQL_ROOT_PASSWORD=rootpassword
ENV MYSQL_DATABASE=benchmark
ENV MYSQL_USER=benchmark
ENV MYSQL_PASSWORD=benchmark

COPY my.cnf /etc/mysql/conf.d/benchmark.cnf
COPY collect-version.sh /usr/local/bin/collect-version.sh
COPY init/01-benchmark-users.sh /docker-entrypoint-initdb.d/01-benchmark-users.sh

RUN chmod +x /usr/local/bin/collect-version.sh /docker-entrypoint-initdb.d/01-benchmark-users.sh

HEALTHCHECK --interval=5s --timeout=3s --start-period=30s --retries=10 \
    CMD /usr/local/bin/collect-version.sh
