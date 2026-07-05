# PostgreSQL 16 for ScratchBird Benchmarks
# Optimized for testing with benchmark database pre-configured

FROM ubuntu:22.04

LABEL maintainer="ScratchBird Benchmarks"
LABEL description="PostgreSQL 16 benchmark engine"

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# PostgreSQL version
ENV PG_VERSION=16
ENV PG_MAJOR=16
ENV PATH=/usr/lib/postgresql/${PG_MAJOR}/bin:$PATH
ENV PG_USER=benchmark
ENV PG_PASSWORD=benchmark
ENV PG_DATABASE=benchmark

# Install dependencies
RUN apt-get update && apt-get install -y \
    wget \
    curl \
    gnupg2 \
    lsb-release \
    locales \
    && rm -rf /var/lib/apt/lists/*

# Add PostgreSQL APT repository
RUN echo "deb http://apt.postgresql.org/pub/repos/apt/ $(lsb_release -cs)-pgdg main" > /etc/apt/sources.list.d/pgdg.list \
    && wget --quiet -O - https://www.postgresql.org/media/keys/ACCC4CF8.asc | apt-key add -

# Install PostgreSQL
RUN apt-get update && apt-get install -y \
    postgresql-${PG_MAJOR} \
    postgresql-client-${PG_MAJOR} \
    postgresql-contrib-${PG_MAJOR} \
    && rm -rf /var/lib/apt/lists/*

# Set locale
RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8

# PostgreSQL directories
ENV PGDATA=/var/lib/postgresql/data
RUN mkdir -p ${PGDATA} /var/run/postgresql \
    && chown -R postgres:postgres ${PGDATA} /var/run/postgresql

# Copy configuration
COPY postgresql.conf /etc/postgresql/${PG_MAJOR}/main/postgresql.conf
COPY pg_hba.conf /etc/postgresql/${PG_MAJOR}/main/pg_hba.conf
COPY collect-version.sh /usr/local/bin/collect-version.sh
RUN chmod +x /usr/local/bin/collect-version.sh

# Copy entrypoint script
COPY docker-entrypoint.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

# Expose PostgreSQL port
EXPOSE 5432

# Health check
HEALTHCHECK --interval=5s --timeout=3s --start-period=10s --retries=5 \
    CMD /usr/local/bin/collect-version.sh

ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["postgres"]
