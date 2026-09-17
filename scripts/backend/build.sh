#!/bin/bash
# build.sh - Build the backend project (Linux/macOS)
# All platforms build through Conan + `cmake --preset` (parity with CI and
# with build.bat on Windows). Each preset installs its dependencies into its
# own build/<preset-name> directory (see CMakePresets.json binaryDir).

set -e

# Load common environment (provides paths.env vars + resolve_cmake_preset)
source "$(dirname "$0")/env_common.sh"

BUILD_TYPE=Release
INSTALL_DEPS=false
SANITIZER=off

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

Show-Help() {
    echo "Usage: ./build.sh [options]"
    echo ""
    echo "Options:"
    echo "  --debug             Build in Debug mode"
    echo "  --release           Build in Release mode (default)"
    echo "  --install-deps      Install the OS build toolchain (compiler, cmake,"
    echo "                        git). C/C++ libraries come from Conan, not apt/brew."
    echo "  --sanitizer=<kind>  Enable a sanitizer for the test target:"
    echo "                        off (default) | thread (TSan) | address (ASan)"
    echo "                        Implies --debug. TSan and ASan are mutually"
    echo "                        exclusive; run two builds to cover both."
    echo "  --tsan              Shortcut for --sanitizer=thread (implies --debug)"
    echo "  --asan              Shortcut for --sanitizer=address (implies --debug)"
    echo "  --help              Show this help"
}

for arg in "$@"; do
    case $arg in
        Debug|Release|RelWithDebInfo|MinSizeRel)
            BUILD_TYPE=$arg
            ;;
        --debug|-debug)
            BUILD_TYPE=Debug
            ;;
        --sanitizer=*)
            SANITIZER="${arg#*=}"
            # Sanitizers require a Debug build with frame pointers/symbols.
            BUILD_TYPE=Debug
            ;;
        --tsan)
            SANITIZER=thread
            BUILD_TYPE=Debug
            ;;
        --asan)
            SANITIZER=address
            BUILD_TYPE=Debug
            ;;
        --install-deps)
            INSTALL_DEPS=true
            ;;
        --help|-h)
            Show-Help
            exit 0
            ;;
    esac
done

case "$SANITIZER" in
    off|thread|address) ;;
    *)
        echo -e "${RED}[Error] --sanitizer must be one of: off | thread | address (got '$SANITIZER')${NC}"
        exit 1
        ;;
esac

PRESET="$(resolve_cmake_preset "$BUILD_TYPE" "$SANITIZER")"
if [ -z "$PRESET" ]; then
    echo -e "${RED}[Error] Could not resolve a CMake preset for this platform.${NC}"
    exit 1
fi
PRESET_DIR="$BUILD_ABS_DIR/$PRESET"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Building Project - preset: $PRESET (config: $BUILD_TYPE)${NC}"
echo -e "${GREEN}========================================${NC}"

# 1. Install OS build toolchain (optional). C/C++ libraries (Drogon, OpenSSL,
#    jsoncpp, libpq, hiredis, ...) are resolved by Conan, not the OS package
#    manager, so this only bootstraps the compiler/cmake/git.
if [ "$INSTALL_DEPS" = true ]; then
    echo -e "${YELLOW}[INFO] Installing OS build toolchain...${NC}"
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        sudo apt-get update
        sudo apt-get install -y git build-essential cmake pkg-config
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        brew install git cmake pkg-config
    else
        echo -e "${RED}[Error] Unsupported OS type: $OSTYPE${NC}"
        exit 1
    fi
    echo -e "${YELLOW}[INFO] Conan is required too: 'pipx install conan' or 'pip install conan'.${NC}"
fi

if ! command -v conan >/dev/null 2>&1; then
    echo -e "${RED}[Error] Conan not found. Install it (e.g. 'pipx install conan') and retry.${NC}"
    exit 1
fi

cd "$PROJECT_DIR"

# CMakeUserPresets.json is a Conan-generated, gitignored artifact whose
# `include` list points at previously-installed build/<dir>/CMakePresets.json
# files. A stale include to a now-missing folder makes `cmake --preset` fail
# to parse, so drop it and let `conan install` regenerate a clean one.
rm -f "$PROJECT_DIR/CMakeUserPresets.json"

echo -e "${YELLOW}[INFO] Installing dependencies with Conan (preset $PRESET)...${NC}"
if [ ! -f "$HOME/.conan2/profiles/default" ]; then
    echo -e "${YELLOW}[INFO] Initializing default conan profile...${NC}"
    conan profile detect
fi
# Linux: use the platform cmake instead of letting tool_requires like
# libcbor's cmake/4.x build from source — the recipe fetches its source
# tarball from GitHub, which 403s the shared anonymous-IP pools of
# GitHub-hosted arm runners (broke the v1.3.1 arm64 image build).
# platform_tool_requires demands an EXACT version: detect the system one.
if [[ "$OSTYPE" == linux* ]]; then
    if ! grep -q '^\[platform_tool_requires\]' "$HOME/.conan2/profiles/default" 2>/dev/null; then
        sys_cmake_ver="$(cmake --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
        if [ -n "$sys_cmake_ver" ]; then
            printf '\n[platform_tool_requires]\ncmake/%s\n' "$sys_cmake_ver" >> "$HOME/.conan2/profiles/default"
        fi
    fi
fi

CONAN_ARGS=(install . --output-folder="build/$PRESET" -s build_type="$BUILD_TYPE" -s compiler.cppstd=17 --build=missing --lockfile-partial)
if [[ "$OSTYPE" == "darwin"* ]]; then
    CONAN_ARGS+=(-s arch=armv8)
fi
conan "${CONAN_ARGS[@]}"

# drogon_create_views()/drogon_create_model() invoke `drogon_ctl` as a bare
# command at BUILD time, so its bin/ dir (from the Conan package) must be on
# PATH. CMakeDeps' Drogon-*-data.cmake records the resolved package folder.
data_file=$(find "build/$PRESET" -maxdepth 1 -name 'Drogon-*-data.cmake' | head -n1)
if [ -n "$data_file" ]; then
    drogon_folder=$(grep -oE 'set\(drogon_PACKAGE_FOLDER_[A-Z]+ "[^"]+"' "$data_file" | sed -E 's/.*"([^"]+)"/\1/' | head -n1)
    if [ -n "$drogon_folder" ] && [ -x "$drogon_folder/bin/drogon_ctl" ]; then
        echo -e "${YELLOW}[INFO] Adding drogon_ctl to PATH: $drogon_folder/bin${NC}"
        export PATH="$drogon_folder/bin:$PATH"
    fi
fi

echo -e "${YELLOW}[INFO] Configuring (cmake --preset $PRESET)...${NC}"
set +e
cmake --preset "$PRESET"
CMAKE_CONFIG_RC=$?
set -e
if [ $CMAKE_CONFIG_RC -ne 0 ]; then
    echo -e "${RED}[Error] CMake configuration failed.${NC}"
    echo -e "${YELLOW}Hint: ensure 'conan install' above succeeded and Conan/CMake are installed.${NC}"
    echo -e "${YELLOW}      One-time toolchain bootstrap: ./manage.sh build-backend --install-deps${NC}"
    exit 1
fi

echo -e "${YELLOW}[INFO] Building (cmake --build --preset $PRESET)...${NC}"
cmake --build --preset "$PRESET" --config "$BUILD_TYPE" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

# Finalize: stage config.json next to the server binary (single-config Unix
# generators put it directly under <preset>/apps/server) and into the tests
# build dir. main.cc has no -c flag; it probes ./config.json relative to CWD.
echo -e "${YELLOW}[INFO] Copying config files...${NC}"
mkdir -p "$PRESET_DIR/$SERVER_BUILD_SUBDIR"
cp "$PROJECT_DIR/$FULLA_SERVER_DIR/$CONFIG_FILE" "$PRESET_DIR/$SERVER_BUILD_SUBDIR/"
mkdir -p "$PRESET_DIR/$TESTS_BUILD_SUBDIR"
cp "$PROJECT_DIR/$FULLA_SERVER_DIR/$CONFIG_FILE" "$PRESET_DIR/$TESTS_BUILD_SUBDIR/config.json"

echo -e "${GREEN}Build Completed Successfully! (preset $PRESET)${NC}"
