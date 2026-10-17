#!/usr/bin/env bash

MODE=${1:-release}

if [ "$MODE" = "debug" ]; then
    SLANG_FLAGS="-g -O0 -line-directive-mode standard"
else
    SLANG_FLAGS="-O3"
fi
set -e

SRC_DIR="shaders"
OUT_DIR="compiledshaders"

mkdir -p "$OUT_DIR"

echo "Compiling Slang shaders..."
echo

# A file defines an entry point if it defines a function of that name. Matching
# the bare name anywhere in the file is not enough: a comment that merely
# mentions cs_scan_blocks made the compiler try to build it out of cull.slang.
has_entry ()
{
    grep -qE "^[[:space:]]*(\[[^]]*\][[:space:]]*)*[A-Za-z_][A-Za-z0-9_<>:*]*[[:space:]]+$1[[:space:]]*\(" "$2"
}

compile_stage ()
{
    local stage="$1"
    local entry="$2"
    local src="$3"
    local out="$4"

    if [ ! -f "$out" ] || [ "$src" -nt "$out" ]; then
        echo "  $stage → $out ($MODE)"

        /opt/shader-slang-bin/bin/slangc "$src" \
            -target spirv \
            -entry "$entry" \
            -stage "$stage" \
            -I shaders \
	    $SLANG_FLAGS \
            ${SLANG_DEFINES:-} \
            -o "$out"
    fi
}
for file in "$SRC_DIR"/*.slang; do

    name=$(basename "$file" .slang)

    # Only compile vertex stage if vs_main exists
    if has_entry vs_main "$file"; then
        compile_stage vertex   vs_main "$file" "$OUT_DIR/$name.vert.spv"
    fi

    # Only compile fragment stage if fs_main exists
    if has_entry fs_main "$file"; then
        compile_stage fragment fs_main "$file" "$OUT_DIR/$name.frag.spv"
    fi

    # Only compile compute stage if cs_main exists
    if has_entry cs_main "$file"; then
        compile_stage compute  cs_main "$file" "$OUT_DIR/$name.comp.spv"
    fi

    # Extra compute entry points. create_compute_pipeline() only ever binds
    # "main", so a file with several kernels needs one .spv per entry.
    for entry in cs_count cs_prefix cs_cull cs_hiz \
                 cs_scan_block cs_scan_blocks cs_emit cs_scatter \
                 cs_sort_clear cs_sort_hist cs_sort_scan cs_sort_scatter; do
        if has_entry "$entry" "$file"; then
            compile_stage compute  "$entry" "$file" "$OUT_DIR/$name.$entry.comp.spv"
        fi
    done

    # Early-Z / depth-only-preZ route fragments: scene.slang (or toon.slang)
    # included with SCENE_NO_DISCARD / SCENE_PREZ set. Explicit entries: the
    # include hides fs_main from has_entry's scan.
    if [ "$file" = "$SRC_DIR/scene_opaque.frag.slang" ] || [ "$file" = "$SRC_DIR/toon_opaque.frag.slang" ] \
        || [ "$file" = "$SRC_DIR/scene_prez.frag.slang" ] || [ "$file" = "$SRC_DIR/toon_prez.frag.slang" ]; then
        # $name already ends in .frag (basename without .slang)
        compile_stage fragment fs_main "$file" "$OUT_DIR/$name.spv"
    fi

done

echo
echo "Done."
