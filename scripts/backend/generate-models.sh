#!/usr/bin/env bash
# generate-models.sh - Generate Drogon ORM models (Linux/macOS)
set -euo pipefail

source "$(dirname "$0")/env_common.sh"

# drogon_ctl comes from the Conan drogon package that build.sh installs;
# discover it from PATH / build output / Conan cache (env_common.sh).
if ! ensure_drogon_ctl; then
    echo "[Hint] Run scripts/backend/build.sh once to install the drogon package, then retry." >&2
    exit 1
fi

# ORM models live in libs/storage-postgres (M2b Task 18; paths.env rebased
# in Phase 4 when the old OAuth2Plugin/ directory was deleted).
MODELS_SRC_DIR="$LIBS_STORAGE_POSTGRES_ABS_DIR/$MODELS_SRC_REL_DIR"
MODELS_INC_DIR="$LIBS_STORAGE_POSTGRES_ABS_DIR/$MODELS_INC_REL_DIR"
MODELS_BACKUP="$LIBS_STORAGE_POSTGRES_ABS_DIR/$MODELS_BACKUP_REL_DIR"

echo ""
echo "========================================"
echo "OAuth2 Plugin Model Generation"
echo "========================================"
echo ""

AUTO_MODE=0
for arg in "$@"; do
    case "$arg" in
        -y|--force) AUTO_MODE=1 ;;
    esac
done

if [ $AUTO_MODE -eq 0 ]; then
    echo "WARNING: This will regenerate ORM models in $MODELS_SRC_DIR"
    read -rp "Press Enter to continue or Ctrl+C to cancel..."
fi

# Backup existing models
if [ -d "$MODELS_SRC_DIR" ]; then
    echo "Backing up existing models to $MODELS_BACKUP..."
    rm -rf "$MODELS_BACKUP"
    mkdir -p "$MODELS_BACKUP"
    cp -r "$MODELS_SRC_DIR"/* "$MODELS_BACKUP/" 2>/dev/null || true
    if ls "$MODELS_INC_DIR"/*.h &>/dev/null; then
        cp "$MODELS_INC_DIR"/*.h "$MODELS_BACKUP/" 2>/dev/null || true
    fi
fi

echo "Generating ORM models..."
mkdir -p "$MODELS_SRC_DIR"

# drogon_ctl reads model.json FROM the target dir and writes output there.
# Run from the repo root and pass the models dir directly -- no ../.. depth
# hack, no dependency on any model.json copy under apps/server.
cd "$PROJECT_DIR"
if [ $AUTO_MODE -eq 1 ]; then
    echo "y" | drogon_ctl create model "$LIBS_STORAGE_POSTGRES_DIR/$MODELS_SRC_REL_DIR"
else
    drogon_ctl create model "$LIBS_STORAGE_POSTGRES_DIR/$MODELS_SRC_REL_DIR"
fi

echo "Moving header files to $MODELS_INC_DIR..."
mkdir -p "$MODELS_INC_DIR"
# Remove old headers
rm -f "$MODELS_INC_DIR"/*.h
# Move generated headers
if ls "$MODELS_SRC_DIR"/*.h &>/dev/null; then
    mv "$MODELS_SRC_DIR"/*.h "$MODELS_INC_DIR/"
fi

echo ""
echo "========================================"
echo "Model generation complete!"
echo "========================================"
