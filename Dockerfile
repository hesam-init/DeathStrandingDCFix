# Build environment for the MinGW-w64 cross-compile (Debian trixie: CMake 3.31, MinGW-w64 GCC 14).
#
#   docker build -t dsfix-build .
#   docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" dsfix-build                  # release build
#   docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" dsfix-build dist VERSION=1.0 # release zip
#
# Arguments are Makefile targets/variables. Output goes to build/docker/<mode>/, kept apart from
# the host's build/mingw/ (a CMake build folder can't be shared between /src and the host path).
# Outside Iran, use the default mirror: docker build --build-arg DEBIAN_MIRROR=http://deb.debian.org/debian ...

FROM debian:trixie

ARG DEBIAN_MIRROR=http://repo.iut.ac.ir/debian

ENV TERM=xterm-256color
ENV DEBIAN_FRONTEND=noninteractive

# ── Debian mirror ────────────────────────────────────────────────────────────
RUN rm /etc/apt/sources.list.d/debian.sources
RUN echo "deb ${DEBIAN_MIRROR}/ trixie main contrib non-free non-free-firmware" > /etc/apt/sources.list

# ── Refresh package index ────────────────────────────────────────────────────
RUN apt update

# ── Build tools (ca-certificates: HTTPS dependency downloads) ────────────────
RUN apt install -y --no-install-recommends \
    make cmake ninja-build ca-certificates

# ── MinGW-w64 cross compiler (posix thread model only, same as Arch's mingw-w64-gcc) ──
RUN apt install -y --no-install-recommends \
    gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix

WORKDIR /src

# BUILD_DIR stays unexpanded here; make resolves $(MODE) itself.
ENTRYPOINT ["make", "BUILD_DIR=build/docker/$(MODE)"]
