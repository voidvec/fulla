# ==========================================
# Unified Dockerfile for fulla
# Supports multiple targets: backend-dev, backend-runtime, frontend-runtime
# ==========================================

# Global Arguments for customization
ARG NODE_VERSION=22-alpine

# --- Stage 1: Backend Build Environment (Base) ---
# C/C++ libraries (Drogon, jsoncpp, libpq, hiredis, ...) come from Conan, not
# apt — parity with scripts/backend/build.sh and CI (_build-test.yml). apt only
# bootstraps the compiler toolchain plus the client tools used by backend-dev.
# GCC 13 toolchain: the 22.04 (GCC 11) linker hit lto1 ICE
# (bytecode stream: expected tag LTO_function) linking fulla-identity-test
# against the conan libcbor package — libcbor landed in v1.1.0 (#142).
# 24.04 matches the CI linux leg and the WSL dev environment where the
# same tree builds clean, so the image now tracks those.
FROM ubuntu:24.04 AS backend-base
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential cmake git pkg-config python3-pip wget ca-certificates \
    libssl-dev uuid-dev zlib1g-dev \
    postgresql-client redis-tools iputils-ping netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

# Conan 2.x resolves all C/C++ dependencies (drogon/1.9.13 per conanfile.py).
# cmake also comes from pip (>= 3.23 for the Conan-generated
# CMakeUserPresets.json schema v4), independent of the base image's apt
# cmake; /usr/local/bin precedes /usr/bin so the pip cmake wins.
# --break-system-packages: PEP 668 marks the 24.04 system python as
# externally managed; in a throwaway builder image installing into it is
# the intended use (a venv + PATH dance would buy nothing here).
RUN pip3 install --no-cache-dir --break-system-packages \
    "conan>=2,<3" "cmake>=3.23" && conan profile detect --force

# --- Stage 2: Backend Development (Target: backend-dev) ---
FROM backend-base AS backend-dev
WORKDIR /app
CMD ["/bin/bash"]

# --- Stage 3: Backend Application Builder ---
# Reuse the canonical build script (Conan install + `cmake --preset
# linux-release` + drogon_ctl PATH wiring) instead of a hand-rolled cmake
# invocation, so the container build cannot drift from host/CI builds.
# The BuildKit cache mount keeps the Conan cache (downloaded sources + built
# binaries, e.g. the boost/drogon source builds) across image rebuilds, so a
# transient network failure does not restart hours of `--build=missing` work.
# The mount shadows the image's /root/.conan2, so re-detect the profile here.
# FULLA_CMAKE_PRESET (build arg, default empty = OS-default preset) lets
# an image build select an alternate preset — e.g. the LTO bench arm via
# compose build.args + `FULLA_CMAKE_PRESET=linux-release-lto`.
FROM backend-base AS backend-builder
WORKDIR /app
COPY . .
ARG FULLA_CMAKE_PRESET=""
ENV FULLA_CMAKE_PRESET=${FULLA_CMAKE_PRESET}
RUN --mount=type=cache,target=/root/.conan2 \
    conan profile detect --force && \
    bash scripts/backend/build.sh Release

# --- Stage 4: Backend Runtime (Target: backend-runtime) ---
# Conan links all C/C++ dependencies statically: `ldd fulla-server` inside
# the image resolves only libstdc++/libm/libgcc_s/libc, which the ubuntu:24.04
# base already ships. The former per-library apt list (libjsoncpp25, libpq5,
# libhiredis0.14, ...) is gone; only TLS roots for outbound HTTPS (SMTP,
# social login) and curl for container healthchecks remain.
FROM ubuntu:24.04 AS backend-runtime
# Mirrors the builder stage's preset choice for the artifact path: an empty
# FULLA_CMAKE_PRESET resolves to the Linux default (linux-release) inside
# build.sh, so the fallback here must match; alternate presets (e.g.
# linux-release-lto) relocate the binary to build/<preset>/apps/server/.
ARG FULLA_CMAKE_PRESET=""
ENV FULLA_CMAKE_PRESET=${FULLA_CMAKE_PRESET:-linux-release}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
# Phase 5 (directory restructure): sources live in apps/server, the binary
# is fulla-server; the in-container layout (/app/config.json, /app/views,
# /app/sql/{migrations,seed}) is kept unchanged. The binary comes from the
# Conan preset build dir (build/<preset>, see CMakePresets.json binaryDir).
COPY --from=backend-builder /app/build/${FULLA_CMAKE_PRESET}/apps/server/fulla-server .
COPY --from=backend-builder /app/apps/server/config/config.prod.json ./config.json
COPY --from=backend-builder /app/apps/server/views ./views
COPY --from=backend-builder /app/apps/server/migrations ./sql/migrations
COPY --from=backend-builder /app/apps/server/seed ./sql/seed
RUN mkdir -p logs uploads
EXPOSE 5555
CMD ["./fulla-server"]

# --- Stage 5: Frontend Builder ---
# Only proxy Node as it was the one failing
FROM node:${NODE_VERSION} AS frontend-builder
# Vite build-time vars (inlined into the SPA bundle via import.meta.env.VITE_*).
# Passed through from docker-compose.prod.yml build.args; default empty so the
# SPA falls back to its in-code defaults when a var is not provided.
ARG VITE_API_BASE_URL=
ARG VITE_CLIENT_ID=fulla-portal
ARG VITE_REDIRECT_URI=
ARG VITE_GITHUB_CLIENT_ID=
ENV VITE_API_BASE_URL=$VITE_API_BASE_URL \
    VITE_CLIENT_ID=$VITE_CLIENT_ID \
    VITE_REDIRECT_URI=$VITE_REDIRECT_URI \
    VITE_GITHUB_CLIENT_ID=$VITE_GITHUB_CLIENT_ID
WORKDIR /app/frontends/user
COPY frontends/user/package*.json ./
RUN npm install
COPY frontends/user/ .
RUN npm run build

# --- Stage 6: Frontend Runtime (Target: frontend-runtime) ---
FROM nginx:stable-alpine AS frontend-runtime
COPY --from=frontend-builder /app/frontends/user/dist /usr/share/nginx/html
COPY frontends/user/nginx.conf /etc/nginx/conf.d/default.conf
EXPOSE 80
CMD ["nginx", "-g", "daemon off;"]
