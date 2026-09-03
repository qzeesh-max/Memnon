FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libboost-all-dev \
    git \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# Copy the project files
COPY . .

# Run the build script
CMD ["/bin/bash", "-c", "mkdir -p build_docker && cd build_docker && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .. && cmake --build . && ctest -V"]
