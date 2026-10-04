#!/usr/bin/env bash
# make_rootfs.sh - build a tiny root filesystem for minibox.
#
# Usage:  sudo ./make_rootfs.sh [DIR]        (default DIR: ./rootfs)
#
# If busybox is installed it is used (smallest result: sudo apt install busybox-static).
# Otherwise a handful of common tools are copied from this machine together with
# the shared libraries they need (found with ldd).

set -euo pipefail

ROOT="${1:-./rootfs}"
mkdir -p "$ROOT"/{bin,sbin,usr/bin,lib,lib64,etc,proc,dev,tmp,sys,root}

copy_libs() {  # copy every shared library a binary needs into the new root
    local bin="$1" lib
    ldd "$bin" 2>/dev/null | grep -oE '/[^ ]+' | while read -r lib; do
        [ -e "$lib" ] || continue
        mkdir -p "$ROOT$(dirname "$lib")"
        cp -L "$lib" "$ROOT$lib"
    done || true
}

if command -v busybox >/dev/null 2>&1; then
    echo "Using busybox from $(command -v busybox)"
    cp "$(command -v busybox)" "$ROOT/bin/busybox"
    copy_libs "$ROOT/bin/busybox"
    for app in $("$ROOT/bin/busybox" --list); do
        [ "$app" = "busybox" ] && continue
        ln -sf busybox "$ROOT/bin/$app"
    done
else
    echo "busybox not found - copying common tools from this machine"
    TOOLS="sh bash ls cat ps hostname sleep id uname mkdir rm cp mv ln head tail grep od dd yes free df mount env wc touch whoami date seq"
    for t in $TOOLS; do
        src="$(command -v "$t" 2>/dev/null || true)"
        if [ -n "$src" ] && [ -f "$src" ]; then
            cp -L "$src" "$ROOT/bin/$t"
            copy_libs "$src"
        fi
    done
fi

cat > "$ROOT/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
EOF
cat > "$ROOT/etc/group" <<'EOF'
root:x:0:
EOF
echo "minibox" > "$ROOT/etc/hostname"

echo "Root filesystem ready in $ROOT ($(du -sh "$ROOT" | cut -f1))"
