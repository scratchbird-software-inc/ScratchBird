# FirebirdSQL 5.0.1 for ScratchBird Benchmarks
# Optimized for testing with benchmark database pre-configured

FROM ubuntu:22.04

LABEL maintainer="ScratchBird Benchmarks"
LABEL description="FirebirdSQL 5.0.1 benchmark engine"

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Firebird version
ENV FB_VERSION=5.0.1
ENV FB_URL=https://github.com/FirebirdSQL/firebird/releases/download/v5.0.1/Firebird-5.0.1.1469-0-linux-x64.tar.gz

# Install dependencies
RUN apt-get update && apt-get install -y \
    wget \
    curl \
    netcat \
    libicu70 \
    libtommath1 \
    libncurses5 \
    && rm -rf /var/lib/apt/lists/*

# Download and install Firebird
RUN wget -q "$FB_URL" -O /tmp/firebird.tar.gz \
    && cd /tmp \
    && tar -xzf firebird.tar.gz \
    && cd Firebird-*-linux-x64 \
    && ./install.sh -silent \
    && cd / \
    && rm -rf /tmp/firebird.tar.gz /tmp/Firebird-*

# Set Firebird environment
ENV FIREBIRD_HOME=/opt/firebird
ENV PATH=$FIREBIRD_HOME/bin:$PATH
ENV LD_LIBRARY_PATH=$FIREBIRD_HOME/lib:$LD_LIBRARY_PATH

# Create benchmark user and database
ENV FIREBIRD_DATABASE=benchmark.fdb
ENV FIREBIRD_USER=benchmark
ENV FIREBIRD_PASSWORD=benchmark

# Copy configuration and scripts
COPY firebird.conf $FIREBIRD_HOME/firebird.conf
COPY collect-version.sh /usr/local/bin/collect-version.sh
RUN chmod +x /usr/local/bin/collect-version.sh

# Create data directory
RUN mkdir -p /firebird/data && chown -R firebird:firebird /firebird

# Copy entrypoint script
COPY docker-entrypoint.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

# Expose Firebird port
EXPOSE 3050

# Health check
HEALTHCHECK --interval=5s --timeout=3s --start-period=10s --retries=5 \
    CMD /usr/local/bin/collect-version.sh

ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["firebird"]
