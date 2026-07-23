# Reproduces the Linux CI job locally. Same distribution as the runner and the
# same package list as .github/workflows/ci.yml, so a dependency missing there
# shows up here instead of costing a CI run.
#
#   docker build -f tools/checks/linux.Dockerfile .
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq \
      build-essential cmake git pkg-config \
      libasound2-dev libpulse-dev \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev \
      libxi-dev libxss-dev libxtst-dev libxkbcommon-dev \
      libwayland-dev wayland-protocols libdecor-0-dev \
      libdrm-dev libgbm-dev libgl1-mesa-dev libegl1-mesa-dev \
      libdbus-1-dev libudev-dev libibus-1.0-dev \
    && rm -rf /var/lib/apt/lists/*
COPY . /src
RUN cmake -S /src -B /build -DCOSMO_WERROR=ON -DCMAKE_BUILD_TYPE=Release
RUN cmake --build /build -j4
RUN cd /build && ctest --output-on-failure
RUN ls -la /build/cosmo /build/cosmo1 /build/cosmo2 /build/cosmo3 /build/imgview
