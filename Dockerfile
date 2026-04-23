# ── Stage 1: Builder ─────────────────────────────────────────
FROM ubuntu:22.04 AS builder

# Install all required C++ build tools and libraries
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libboost-system-dev \
    libboost-coroutine-dev \
    libboost-context-dev \
    libssl-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy all source files
COPY . .

# Configure and build the C++ project
RUN cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
RUN cmake --build build -j$(nproc)

# ── Stage 2: Runtime ─────────────────────────────────────────
FROM ubuntu:22.04

# FIX: Install runtime libraries for Boost and OpenSSL so the binary can run
RUN apt-get update && apt-get install -y \
    libboost-system1.74.0 \
    libboost-coroutine1.74.0 \
    libboost-context1.74.0 \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

# HF Spaces requires the app to run as user 1000
RUN useradd -m -u 1000 appuser

WORKDIR /app

# Copy the compiled binary from the builder stage
COPY --from=builder --chown=appuser:appuser /app/build/collabdocs ./app_server

# Copy your frontend file so the C++ server can serve it
COPY --chown=appuser:appuser index.html .

USER appuser

# HF Spaces exposes port 7860
EXPOSE 7860

# Start the C++ executable
CMD ["./app_server"]
