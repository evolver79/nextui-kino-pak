# Debian bullseye arm64: glibc 2.31, none newer than the TrimUI Brick's. FFmpeg, libass and libmpv are built from
# source as static libraries, so the pak needs only what the Brick already has (SDL2, SDL2_ttf, zlib, libc).
FROM --platform=linux/arm64 debian:bullseye
RUN printf '%s\n' "deb http://archive.debian.org/debian bullseye main" \
      "deb http://archive.debian.org/debian bullseye-updates main" \
      "deb http://archive.debian.org/debian-security bullseye-security main" > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends gcc g++ make pkg-config git ca-certificates file xz-utils curl \
      nasm python3-pip ninja-build zlib1g-dev libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev \
 && pip3 install meson==1.2.3 \
 && git config --global --add safe.directory '*'
