#!/usr/bin/env bash
# The Linux plugin's dependency contract (AGENTS.md): it may need libc, libm,
# the C++ runtime (static, so absent), xcb and the system font engine — and
# nothing else. Fails when a new NEEDED entry or an exported symbol appears.
#
# libpthread / libdl are part of glibc. They appear as NEEDED only before
# glibc 2.34 (merged into libc since); the SDK links -lpthread -ldl and the
# plugin uses std::thread (the bank rescan) and dladdr (the factory path).
#
# Usage: tools/check_deps.sh <path to OxoSynth2.so>
set -euo pipefail
SO="${1:?usage: check_deps.sh <plugin.so>}"

allowed_needed="libc.so.6 libm.so.6 ld-linux-x86-64.so.2 ld-linux-aarch64.so.1 libxcb.so.1 libxcb-keysyms.so.1 libfreetype.so.6 libfontconfig.so.1 libz.so.1 libpthread.so.0 libdl.so.2"
allowed_exports="GetPluginFactory ModuleEntry ModuleExit clap_entry"

fail=0
for lib in $(objdump -p "$SO" | awk '/NEEDED/ {print $2}'); do
    case " $allowed_needed " in
        *" $lib "*) ;;
        *) echo "check_deps: unexpected dependency: $lib" >&2; fail=1 ;;
    esac
done
for sym in $(nm -D --defined-only "$SO" | awk '{print $3}'); do
    case " $allowed_exports " in
        *" $sym "*) ;;
        *) echo "check_deps: exported symbol not allowed: $sym" >&2; fail=1 ;;
    esac
done
if [ "$fail" -ne 0 ]; then
    echo "check_deps: FAILED" >&2
    exit 1
fi
echo "check_deps: ok ($(objdump -p "$SO" | grep -c NEEDED) dependencies, $(nm -D --defined-only "$SO" | wc -l) exports)"
