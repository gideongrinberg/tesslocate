FROM debian:bookworm-slim

# Install system dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    curl \
    zip \
    unzip \
    tar \
    wget \
    pkg-config \
    libomp-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install cmake
RUN wget https://github.com/Kitware/CMake/releases/download/v4.0.2/cmake-4.0.2-linux-x86_64.tar.gz && \
    tar -xzf cmake-4.0.2-linux-x86_64.tar.gz && mv cmake-4.0.2-linux-x86_64 /opt/cmake && \
    ln -sf /opt/cmake/bin/cmake /usr/local/bin/cmake && \
    ln -sf /opt/cmake/bin/ccmake /usr/local/bin/ccmake && \
    ln -sf /opt/cmake/bin/cpack /usr/local/bin/cpack && \
    ln -sf /opt/cmake/bin/ctest /usr/local/bin/ctest

# Install vcpkg
RUN git clone https://github.com/Microsoft/vcpkg.git /opt/vcpkg
WORKDIR /opt/vcpkg
RUN ./bootstrap-vcpkg.sh

# Set vcpkg environment variables
ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="$VCPKG_ROOT:$PATH"

# Create app directory
WORKDIR /app

# Copy project files
COPY CMakeLists.txt .
COPY vcpkg.json .
COPY main.cpp .
COPY external/ external/

# Configure vcpkg toolchain
ENV CMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake

# Install dependencies and build project
RUN cmake -B build -S . \
    -DCMAKE_TOOLCHAIN_FILE=$CMAKE_TOOLCHAIN_FILE \
    -DCMAKE_BUILD_TYPE=Release

RUN cmake --build build --config Release

# Set the executable as the entrypoint
ENTRYPOINT ["./build/tesslocate"]