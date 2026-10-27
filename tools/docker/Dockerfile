# syntax=docker/dockerfile:1
#
# Build container for the MeowKit ESP32-S3 target (`meowkits3`).
#
# The layering is deliberate, and it follows two rules:
#
#   1. Anything APT can provide comes from APT, in its own layer, so the layer
#      caches and can be reused by a stage that does not build.
#   2. Anything that is *not* a distribution package -- the Xtensa toolchain and
#      ESP-IDF -- is installed by the repository's own `tools/rockboxdev.sh`, the
#      upstream convention for Rockbox toolchains. Nothing here hand-rolls an IDF
#      install, so the same commands work on a host that is not this container.
#      `--target=e` installs the prebuilt xtensa-esp-elf release and then clones
#      and installs ESP-IDF (tools/rockboxdev.sh:1172-1184, :441-469).
#
# Build stages:
#
#   rb-base         bare Ubuntu + CA certs
#   rb-deps         core build dependencies (APT)
#   rb-deps-manual  LaTeX, needed only for the manual PDF (APT)
#   rb-src          checkout of the branch
#   rb-toolchain    xtensa-esp-elf + ESP-IDF, via tools/rockboxdev.sh
#   rb-env          the build environment; entrypoint builds and emits artifacts
#   rb-artifacts    artifacts only (--target=rb-artifacts, needs
#                   --build-arg BUILD_AT_IMAGE_BUILD=yes)
#
# There is no CI here by design: `docker run` with an output directory mounted
# at /out is the whole interface.

ARG BASE_IMAGE=ubuntu:26.04

###############################################################################
# 1. Bare upstream Ubuntu
###############################################################################
FROM ${BASE_IMAGE} AS rb-base

ARG DEBIAN_FRONTEND=noninteractive
ENV LC_ALL=C.UTF-8

RUN apt-get update \
 && apt-get -y --no-install-recommends install ca-certificates \
 && rm -rf /var/lib/apt/lists/*

###############################################################################
# 2. Core build dependencies
#
# The list is not a guess. It is the union of:
#   - tools/rockboxdev.sh:91, which names its own requirements:
#     gcc g++ xz bzip2 gzip make patch makeinfo automake libtool autoconf flex
#     bison  (makeinfo comes from texinfo)
#   - tools/rockboxdev.sh:511-517, which refuses to install ESP-IDF without
#     git, cmake, ninja and python3
#   - tools/buildzip.pl, which calls `zip` and the `install` command
#   - tools/voice.pl:29, which `use`s Encode::Locale. That is not in the base
#     `perl` package, and without it the voice pass dies with "Can't locate
#     Encode/Locale.pm in @INC" after having already built rbspeexenc, voicefont
#     and wavtrim -- so the failure looks like a codec build problem.
#   - tools/configure, which needs no downloads for this target and calls the
#     host compiler to build tools/. espeak-ng is here because the default build
#     is a voice build: configure's --tts=espeak falls back to espeak-ng when the
#     old name is absent (tools/configure:1423-1424), and without it the card
#     payload silently ships without the .voice file -- 2.5 MB and two files
#     shorter than what the device actually gets.
###############################################################################
FROM rb-base AS rb-deps

RUN apt-get update \
 && apt-get -y --no-install-recommends install \
      build-essential \
      make \
      patch \
      file \
      pkg-config \
      texinfo \
      automake \
      autoconf \
      libtool \
      libtool-bin \
      flex \
      bison \
      git \
      perl \
      libencode-locale-perl \
      zip \
      unzip \
      wget \
      curl \
      xz-utils \
      bzip2 \
      gzip \
      python3 \
      python3-pip \
      python3-venv \
      python3-dev \
      libffi-dev \
      libssl-dev \
      cmake \
      ninja-build \
      ccache \
      espeak-ng \
      libusb-1.0-0 \
      libudev1 \
 && rm -rf /var/lib/apt/lists/*

###############################################################################
# 3. Manual dependencies, in their own layer
#
# manual/preamble.tex uses scrreprt (Koma), inputenc/fontenc/lmodern, tabularx,
# multirow, multicol, scrhack, float, hyperref, enumitem, optional, xcolor with
# [table], fancyhdr, graphicx, verbatim, makeidx, fancyvrb, booktabs, longtable
# and url. Mapping each of those against a host that builds the manual cleanly
# lands on texlive-latex-recommended + texlive-latex-extra +
# texlive-fonts-recommended -- plus `lmodern`, which both distributions split out
# of texlive-latex-recommended into its own package. Without it pdflatex stops at
# "File `lmodern.sty' not found" and produces no PDF. Only `manual-pdf` needs this
# layer, so it is separable: build --target=rb-deps if you want an image without
# several hundred MB of TeX.
###############################################################################
FROM rb-deps AS rb-deps-manual

RUN apt-get update \
 && apt-get -y --no-install-recommends install \
      texlive-latex-recommended \
      texlive-latex-extra \
      texlive-fonts-recommended \
      lmodern \
 && rm -rf /var/lib/apt/lists/*

###############################################################################
# 4. Checkout
#
# RB_REPO and RB_BRANCH are ARGs so the URL can move when the port goes upstream
# without editing this file. The clone keeps .git: tools/version.sh reads HEAD
# through git, so a shallow clone is fine but a source tarball would not be.
###############################################################################
FROM rb-deps-manual AS rb-src

ARG RB_REPO=https://gitea.max.optiplexus.org/jhohertz/rockbox
ARG RB_BRANCH=esp32-s3-idf
# BuildKit keys a layer on its command and its ARG values, not on the remote's
# content -- so a cached clone would silently survive every push. Pass
# `--build-arg RB_REFRESH=$(date +%s)` to force a fresh checkout.
ARG RB_REFRESH=0

RUN git clone --depth 1 --branch "${RB_BRANCH}" "${RB_REPO}" /src/rockbox \
 && git -C /src/rockbox rev-parse HEAD > /src/rockbox-commit \
 && echo "refresh=${RB_REFRESH}"

###############################################################################
# 5. Toolchains, via the repository's own installer
#
# RBDEV_PREFIX is the single root for everything Rockbox needs: rockboxdev puts
# the Xtensa toolchain under it and clones ESP-IDF to $prefix/esp-idf
# (tools/rockboxdev.sh:441-469). IDF_TOOLS_PATH is where IDF's install.sh puts
# its own version-pinned copies, kept under a predictable path so the layer is
# self-contained. RBDEV_BUILD must not exist beforehand -- the script treats a
# pre-existing build directory as a security problem (tools/rockboxdev.sh:35-37).
###############################################################################
FROM rb-src AS rb-toolchain

ARG RBDEV_PREFIX=/opt/rockbox-dev
ARG RBDEV_ESP_TOOLCHAIN=esp-14.2.0_20260121
ARG RBDEV_ESP_IDF=v5.5.5
ARG RBDEV_ESP_IDF_CORE=esp32s3

ENV RBDEV_PREFIX=${RBDEV_PREFIX} \
    IDF_TOOLS_PATH=/opt/esp/tools \
    RB_TARGET=meowkits3

# Only the *download scratch* is a cache mount. IDF_TOOLS_PATH is not scratch:
# install.sh extracts its tools and creates its Python virtualenv there, so it is
# installed state, and a cache mount on that path would leave the image without a
# venv -- `export.sh` then fails at run time with
# "ESP-IDF Python virtual environment ... not found". RBDEV_DOWNLOAD really is
# scratch (rockboxdev extracts into a build dir and installs into RBDEV_PREFIX),
# so caching it is safe and saves the 168 MB archive on every re-clone.
RUN --mount=type=cache,target=/var/cache/rbdev-dl \
    RBDEV_ESP_TOOLCHAIN=${RBDEV_ESP_TOOLCHAIN} \
    RBDEV_ESP_IDF=${RBDEV_ESP_IDF} \
    RBDEV_ESP_IDF_CORE=${RBDEV_ESP_IDF_CORE} \
    RBDEV_DOWNLOAD=/var/cache/rbdev-dl \
    RBDEV_BUILD=/tmp/rbdev-build \
    /src/rockbox/tools/rockboxdev.sh --target=e \
 && rm -rf /tmp/rbdev-build

###############################################################################
# 6. Build environment
#
# The entrypoint configures, builds and copies the artifacts to $OUT_DIR
# (default /out), which is what you mount from the host. It is idempotent: an
# existing build directory is reused, so a container left running with the
# source mounted can rebuild incrementally.
#
# BUILD_AT_IMAGE_BUILD=yes makes the build happen during `docker build` instead,
# into /artifacts -- which is what the rb-artifacts stage needs.
###############################################################################
FROM rb-toolchain AS rb-env

ARG BUILD_AT_IMAGE_BUILD=no
ARG OUT_DIR=/artifacts
# Passed through as ENV so it applies to both paths: the build-time RUN below and
# the entrypoint at container run time.
ARG SKIP_MANUAL=
ENV SKIP_MANUAL=${SKIP_MANUAL}

# The build script travels with the repository, so the container uses the copy
# from the checked-out branch rather than a file from the build context -- the
# two cannot drift.
RUN cp /src/rockbox/tools/docker/rb-build-artifacts.sh /usr/local/bin/rb-build-artifacts.sh \
 && chmod +x /usr/local/bin/rb-build-artifacts.sh

RUN if [ "${BUILD_AT_IMAGE_BUILD}" = "yes" ]; then \
        OUT_DIR=${OUT_DIR} /usr/local/bin/rb-build-artifacts.sh; \
    fi

ENV OUT_DIR=/out \
    BUILD_DIR=/work/build-meowkits3 \
    REPO=/src/rockbox

ENTRYPOINT ["/usr/local/bin/rb-build-artifacts.sh"]

###############################################################################
# 7. Artifacts only
#
# `docker build --target=rb-artifacts --build-arg BUILD_AT_IMAGE_BUILD=yes`
# gives an image whose only content is the build output -- useful for handing a
# verified set to someone else, or for a release step that does not want a
# compiler in the image.
###############################################################################
FROM rb-base AS rb-artifacts

ARG OUT_DIR=/artifacts
COPY --from=rb-env ${OUT_DIR}/ /out/
ENTRYPOINT ["/bin/sh", "-c", "ls -l /out && cat /out/SHA256SUMS"]
