#!/usr/bin/env python3
# Generate one header containing all SPIR-V binaries.
# Array names are derived from filenames and shader stages.
# The lookup function is generated too, avoiding manual registries.

import pathlib
import re
import struct
import sys

output = pathlib.Path(sys.argv[1])
paths = sorted(pathlib.Path(p) for p in sys.argv[2:])

lines = [
    "#pragma once",
    "#include <stdint.h>",
    "#include <stddef.h>",
    "#include <string.h>",
    "",
    "typedef struct {",
    "    const char *path;",
    "    const uint32_t *data;",
    "    size_t size;",
    "} EmbeddedShader;",
    "",
]

entries = []

for path in paths:
    raw = path.read_bytes()

    if not raw or len(raw) % 4:
        raise SystemExit(f"Invalid SPIR-V: {path}")

    # editor.vert.spv -> editor_vert_vs
    # scene3d.comp.spv -> scene3d_comp_cs
    stem = path.name.removesuffix(".spv")
    stage = {
        "vert": "vs",
        "frag": "fs",
        "comp": "cs",
        "geom": "gs",
        "tesc": "tcs",
        "tese": "tes",
    }

    parts = stem.split(".")
    suffix = stage.get(parts[-1], parts[-1])
    base = "_".join(parts[:-1]) if len(parts) > 1 else stem
    name = re.sub(r"\W", "_", f"{base}_{suffix}")

    words = struct.unpack(f"<{len(raw) // 4}I", raw)

    lines.append(
        f"static const uint32_t {name}[] "
        f"__attribute__((aligned(4))) = {{"
    )

    for i in range(0, len(words), 8):
        lines.append(
            "    " + ", ".join(f"0x{x:08x}u" for x in words[i:i+8]) + ","
        )

    lines.extend(["};", ""])

    entries.append(
        f'    {{"{path.as_posix()}", {name}, {len(raw)}}},'
    )

lines.extend([
    "static const EmbeddedShader embedded_shaders[] = {",
    *entries,
    "};",
    "",
    "static inline int embedded_shader_find(",
    "    const char *path, const void **code, size_t *size)",
    "{",
    "    for (size_t i = 0;",
    "         i < sizeof(embedded_shaders) / sizeof(embedded_shaders[0]);",
    "         ++i) {",
    "        if (strcmp(path, embedded_shaders[i].path) == 0) {",
    "            *code = embedded_shaders[i].data;",
    "            *size = embedded_shaders[i].size;",
    "            return 1;",
    "        }",
    "    }",
    "    return 0;",
    "}",
])

output.parent.mkdir(parents=True, exist_ok=True)
output.write_text("\n".join(lines) + "\n")
