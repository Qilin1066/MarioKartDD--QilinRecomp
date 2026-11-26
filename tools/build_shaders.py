#!/usr/bin/env python3
"""Compile mkdd/shaders/ to SPIR-V and write mkdd/src/shaders.inc;
likewise host/shaders/ (the window's picture) to host/src/shaders.inc and
host/shaders/post/ (SMAA and FSR 1, before the picture is shown) to
host/src/post_shaders.inc.

The generated files are committed, so building the game needs no shader
compiler; run this after changing a shader. It needs glslang
(glslangValidator): one on the PATH, the one named by --compiler or by
GLSLANG_VALIDATOR in the environment, or the one CMake builds with
-DQILIN_SHADER_COMPILER=ON (target qilin_shader_compiler), which is found in the
build folder.

Each header records a SHA-256 of every source in its folder. `--check` (CI)
fails if that hash doesn't match the sources, meaning someone changed a
shader without regenerating. It needs no compiler, because SPIR-V bytes
differ between glslang versions. If one is found, it also compiles each
shader, so a broken one fails too.

The post-processing shaders include their authors' own sources, which are
not in the repository: host/shaders/post/third_party.txt pins each by URL and
SHA-256. They are taken from a build folder that has fetched them (CMake does,
when it configures) or fetched here.
"""
import hashlib
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
# (source, name in the generated file, extra glslang arguments)
SHADERS = [
    ("gx.vert", "kGxVert", []),
    ("gx.frag", "kGxFrag", []),
    # The same TEV, writing depth: for Z textures with the Z test after texturing.
    # A shader that writes depth turns off the GPU's early depth test, so only
    # the draws that need it use this one.
    ("gx.frag", "kGxFragDepth", ["-DZTEX_DEPTH"]),
    # Specialised to a draw's shape (renderer.cpp), one per TEV stage count, so
    # a driver compiles only the stages the draw has.
    ("gx.frag", "kGxFragStages1", ["-DSTAGES=1"]),
    ("gx.frag", "kGxFragStages2", ["-DSTAGES=2"]),
    ("gx.frag", "kGxFragStages3", ["-DSTAGES=3"]),
    ("gx.frag", "kGxFragStages4", ["-DSTAGES=4"]),
    ("gx.frag", "kGxFragStages5", ["-DSTAGES=5"]),
    ("gx.frag", "kGxFragStages6", ["-DSTAGES=6"]),
    ("gx.frag", "kGxFragStages7", ["-DSTAGES=7"]),
    ("gx.frag", "kGxFragStages8", ["-DSTAGES=8"]),
    ("gx.frag", "kGxFragStages9", ["-DSTAGES=9"]),
    ("gx.frag", "kGxFragStages10", ["-DSTAGES=10"]),
    ("gx.frag", "kGxFragStages11", ["-DSTAGES=11"]),
    ("gx.frag", "kGxFragStages12", ["-DSTAGES=12"]),
    ("gx.frag", "kGxFragStages13", ["-DSTAGES=13"]),
    ("gx.frag", "kGxFragStages14", ["-DSTAGES=14"]),
    ("gx.frag", "kGxFragStages15", ["-DSTAGES=15"]),
    ("gx.frag", "kGxFragStages16", ["-DSTAGES=16"]),
    ("fullscreen.vert", "kFullscreenVert", []),
    ("clear.frag", "kClearFrag", []),
    ("copy.frag", "kCopyFrag", []),
]
HOST_SHADERS = [
    ("present.vert", "kPresentVert", []),
    ("plain.frag", "kPlainFrag", []),
    ("crt.frag", "kCrtFrag", []),
]
# SMAA's three passes, each a vertex and a fragment shader from the one source; FSR's two.
POST_SHADERS = [
    ("smaa.glsl", f"kSmaa{p}{stage.capitalize()}", [f"-DPASS={p}", "-DVERTEX" if stage == "vert" else "-DFRAGMENT", "-S", stage])
    for p in (1, 2, 3) for stage in ("vert", "frag")
] + [
    ("fsr.comp", "kFsrEasu", ["-DEASU"]),
    ("fsr.comp", "kFsrRcas", ["-DRCAS"]),
]
# (sources, generated file, shaders)
GROUPS = [
    (ROOT / "mkdd/shaders", ROOT / "mkdd/src/shaders.inc", SHADERS),
    (ROOT / "host/shaders", ROOT / "host/src/shaders.inc", HOST_SHADERS),
    (ROOT / "host/shaders/post", ROOT / "host/src/post_shaders.inc", POST_SHADERS),
]


def sources_hash(src: pathlib.Path, shaders) -> str:
    h = hashlib.sha256(repr(shaders).encode())  # the variants too, not just the files
    for f in sorted(src.iterdir()):
        if f.is_file():
            h.update(f.name.encode() + b"\0")
            h.update(f.read_bytes().replace(b"\r\n", b"\n") + b"\0")
    return h.hexdigest()


def find_compiler():
    """The glslang to use, or None."""
    exe = ".exe" if os.name == "nt" else ""
    named = None
    if "--compiler" in sys.argv:
        named = sys.argv[sys.argv.index("--compiler") + 1]
    named = named or os.environ.get("GLSLANG_VALIDATOR")
    if named:
        if not pathlib.Path(named).is_file():
            sys.exit(f"build_shaders: there's no compiler at {named}")
        return named
    for name in ("glslangValidator", "glslang"):
        if found := shutil.which(name):
            return found
    # CMake's own (QILIN_SHADER_COMPILER): somewhere under a build folder's glslang.
    for build in sorted(ROOT.glob("build*")):
        for found in sorted((build / "_deps" / "glslang-build").rglob("glslang" + exe)):
            if found.is_file():
                return str(found)
    return None


def third_party(src: pathlib.Path, tmp: pathlib.Path):
    """The folders to include from for a group with a third_party.txt: its files, each checked against its hash."""
    pins = src / "third_party.txt"
    if not pins.exists():
        return []
    folders = []
    for line in pins.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        digest, path, url = line.split()
        found = None
        for build in sorted(ROOT.glob("build*")):
            f = build / "_deps" / "qilin_post" / path
            if f.is_file() and hashlib.sha256(f.read_bytes()).hexdigest() == digest:
                found = f
                break
        if not found:
            found = tmp / "third_party" / path
            found.parent.mkdir(parents=True, exist_ok=True)
            with urllib.request.urlopen(url, timeout=60) as r:
                data = r.read()
            if hashlib.sha256(data).hexdigest() != digest:
                sys.exit(f"build_shaders: {url} isn't the file third_party.txt pins")
            found.write_bytes(data)
        if found.parent not in folders:
            folders.append(found.parent)
    return [f"-I{f}" for f in folders]


def compile_one(compiler: str, src: pathlib.Path, out: pathlib.Path, extra=()) -> bytes:
    subprocess.run([compiler, "-V", "--target-env", "vulkan1.3", *extra, "-o", str(out), str(src)],
                   check=True, stdout=subprocess.DEVNULL)
    return out.read_bytes()


def check() -> int:
    failed = 0
    compiler = find_compiler()
    for src, target, shaders in GROUPS:
        stamp = f"// Sources: {sources_hash(src, shaders)}"
        lines = target.read_text().splitlines() if target.exists() else []
        if stamp not in lines[:4]:
            print(f"{target.relative_to(ROOT)} is out of date: run tools/build_shaders.py", file=sys.stderr)
            failed = 1
            continue
        if compiler:
            with tempfile.TemporaryDirectory() as tmp:
                include = third_party(src, pathlib.Path(tmp))
                for name, _, extra in shaders:
                    compile_one(compiler, src / name, pathlib.Path(tmp) / "out.spv", [*include, *extra])
    if not compiler:
        print("glslangValidator not found: checked the hashes only")
    return failed


def build(compiler: str, src: pathlib.Path, target: pathlib.Path, shaders) -> None:
    lines = [f"// Generated by tools/build_shaders.py from {src.relative_to(ROOT).as_posix()}/: do not edit.",
             f"// Sources: {sources_hash(src, shaders)}", "#pragma once", "#include <cstdint>", ""]
    with tempfile.TemporaryDirectory() as tmp:
        include = third_party(src, pathlib.Path(tmp))
        for name, ident, extra in shaders:
            b = compile_one(compiler, src / name, pathlib.Path(tmp) / "out.spv", [*include, *extra])
            words = struct.unpack(f"<{len(b) // 4}I", b)
            lines.append(f"inline constexpr uint32_t {ident}[] = {{")
            for i in range(0, len(words), 8):
                lines.append("    " + ", ".join(f"0x{w:08x}" for w in words[i:i + 8]) + ",")
            lines.append("};")
    target.write_text("\n".join(lines) + "\n")


def main() -> int:
    if "--check" in sys.argv:
        return check()
    compiler = find_compiler()
    if not compiler:
        sys.exit("build_shaders: no glslang found: put glslangValidator on the PATH, name one with --compiler, or build "
                 "CMake's (-DQILIN_SHADER_COMPILER=ON, target qilin_shader_compiler)")
    # --only <generated file's name>: one group (the others keep the bytes their compiler gave them).
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    for src, target, shaders in GROUPS:
        if only is None or target.name == only:
            build(compiler, src, target, shaders)
    return 0


if __name__ == "__main__":
    sys.exit(main())
