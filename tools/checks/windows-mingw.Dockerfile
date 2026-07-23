# Cross-compiles the Windows target with MinGW-w64, which is the same compiler
# family the Windows CI job uses. Catches Windows-only compile errors without a
# Windows machine or a CI run.
#
#   docker build -f tools/checks/windows-mingw.Dockerfile .
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq \
      build-essential cmake git pkg-config mingw-w64 \
    && rm -rf /var/lib/apt/lists/*
COPY . /src
RUN cmake -S /src -B /build \
      -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-mingw64.cmake \
      -DCOSMO_WERROR=ON -DCMAKE_BUILD_TYPE=Release
RUN cmake --build /build -j4
RUN ls -la /build/*.exe
