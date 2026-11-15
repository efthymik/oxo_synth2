#!/usr/bin/env bash
# The alternative Linux build (the LICE panel): builds and validates
# OxoSynth2.vst3 in linux-port-lice/build/VST3/Release, installs nothing
# unless asked (--install copies to ~/.vst3).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/linux-port-lice"
BUILD_DIR="$PROJECT_DIR/build"
# Bundle name from Identity.xcconfig (the local override wins, like in CMake).
IDENTITY_FILES=("$SCRIPT_DIR/Identity.xcconfig")
[[ -f "$SCRIPT_DIR/Identity.local.xcconfig" ]] && IDENTITY_FILES+=("$SCRIPT_DIR/Identity.local.xcconfig")
PRODUCT_NAME="$(sed -n 's/^OXO_PRODUCT_NAME[[:space:]]*=[[:space:]]*//p' "${IDENTITY_FILES[@]}" \
  | tail -1 | sed 's/[[:space:]]*$//')"
PLUGIN_BUNDLE="$BUILD_DIR/VST3/Release/${PRODUCT_NAME// /}.vst3"
VALIDATOR="$BUILD_DIR/bin/Release/validator"

INSTALL=0 # Windows equivalent installs nothing; --install copies to ~/.vst3 for host tests
if [[ -n "${1:-}" ]]; then
  case "$1" in
    --install) INSTALL=1 ;;
    *) echo "Usage: build_linux_lice.sh [--install]" >&2; exit 2 ;;
  esac
fi

JOBS="$(nproc 2>/dev/null || echo 4)"

# Always start fresh. The build dir can be shared between machines (a VM and
# its host, a network mount), and CMake then caches the other machine's
# absolute library paths — a stale cache fails at link time with "No rule to
# make target". A clean configure costs seconds; a stale one costs an hour.
rm -rf "$BUILD_DIR"

echo "==> Configuring"
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release > "$BUILD_DIR.configure.log" 2>&1 \
  || { tail -40 "$BUILD_DIR.configure.log"; exit 1; }

echo "==> Building (Release, $JOBS jobs)"
cmake --build "$BUILD_DIR" -j "$JOBS" > "$BUILD_DIR.build.log" 2>&1 \
  || { grep -E "error:|Error [0-9]" "$BUILD_DIR.build.log" | head -40; exit 1; }

[[ -d "$PLUGIN_BUNDLE" ]] || { echo "ERROR: built plugin not found at $PLUGIN_BUNDLE" >&2; exit 1; }
echo "==> Built $PLUGIN_BUNDLE"

echo "==> Checking the dependency contract"
"$SCRIPT_DIR/tools/check_deps.sh" "$PLUGIN_BUNDLE/Contents/x86_64-linux/$(basename "${PLUGIN_BUNDLE%.vst3}").so"

if [[ -x "$VALIDATOR" ]]; then
  echo "==> Running validator"
  HOME_DIR="$(mktemp -d)"
  # HOME=$(mktemp -d): keep the developer's bank library out of the validator.
  if HOME="$HOME_DIR" "$VALIDATOR" "$PLUGIN_BUNDLE"; then
    echo "==> Validator passed"
  else
    echo "ERROR: validator failed" >&2
    exit 1
  fi
  rm -rf "$HOME_DIR"
fi

if [[ $INSTALL -eq 1 ]]; then
  mkdir -p "$HOME/.vst3"
  rm -rf "$HOME/.vst3/$(basename "$PLUGIN_BUNDLE")"
  cp -R "$PLUGIN_BUNDLE" "$HOME/.vst3/"
  echo "==> Installed $HOME/.vst3/$(basename "$PLUGIN_BUNDLE")"
fi
