#!/bin/sh
# Build doomgeneric for Cardinal; as a wasm32-wasi guest -> lisp/data/doom.wasm,
# the guest run by lisp/lib/wasm-doom.clp on the in-OS interpreter (Phase 5 of
# notes/core/wasm-guests.md).
#
# We do NOT vendor the (GPL) doomgeneric tree into this (MIT) repo: only our
# backend (doomgeneric_cardinal.c) and this script are committed. The upstream
# sources are cloned into ./upstream/ on first run (git-ignored). The bundled
# wasi-sdk clang is used (its compiler-rt has the wasm builtins system clang
# lacks); see notes/core/wasm-guests.md for fetching it.
set -e
cd "$(dirname "$0")"
HERE=$(pwd)
ROOT=$(git rev-parse --show-toplevel)

WASI_CLANG="$ROOT/.devenv/wasi-sdk/bin/clang"
[ -x "$WASI_CLANG" ] || { echo "missing $WASI_CLANG (re-fetch wasi-sdk, see wasm-guests notes)"; exit 1; }

# First run: clone doomgeneric (ozkl/doomgeneric) into ./upstream/.
if [ ! -f upstream/doomgeneric.h ]; then
  echo "[doom] fetching doomgeneric -> upstream/"
  rm -rf upstream
  git clone --depth 1 https://github.com/ozkl/doomgeneric.git upstream-clone
  mkdir -p upstream
  cp upstream-clone/doomgeneric/*.c upstream-clone/doomgeneric/*.h upstream/
  rm -rf upstream-clone
fi

OUT="$ROOT/lisp/data/doom.wasm"

# -DNORMALUNIX -> the POSIX path (plain fopen, no win32). 640x400 is the
# doomgeneric.h default. Initial memory 32 MiB, max 64 MiB to match the
# interpreter's WASM_MAX_RESERVE_PAGES cap (the module is reserved at max).
# --strip-all drops the DWARF the toolchain emits (smaller initrd).
CFLAGS="-O2 --target=wasm32-wasi -DNORMALUNIX -Wno-everything -I$HERE/upstream"
LDFLAGS="-Wl,-z,stack-size=1048576 \
  -Wl,--initial-memory=33554432 -Wl,--max-memory=67108864 \
  -Wl,--strip-all \
  -Wl,--export=__heap_base -Wl,--export=__data_end"

# The Doom translation units (the upstream Makefile's SRC_DOOM set) plus our
# backend. Excludes the allegro/SDL/xlib/win sound+video backends and mus2mid
# (its own main) so no host multimedia headers are pulled in.
DOOM_SRCS="am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main \
d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom \
i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config \
m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor \
p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight \
p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs \
r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video \
wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video dummy \
doomgeneric"

SRCS=""
for s in $DOOM_SRCS; do SRCS="$SRCS upstream/$s.c"; done
SRCS="$SRCS doomgeneric_cardinal.c"

echo "[doom] compiling -> $OUT"
# shellcheck disable=SC2086
"$WASI_CLANG" $CFLAGS $LDFLAGS $SRCS -o "$OUT" -lm
echo "[doom] built $OUT ($(wc -c < "$OUT") bytes)"

OBJDUMP="$ROOT/.devenv/bin/wasm-objdump"
if [ -x "$OBJDUMP" ]; then
  echo "[doom] cardinal/wasi imports:"
  "$OBJDUMP" -j Import -x "$OUT" | sed -n '/- func/p' | sed 's/^/    /'
fi
