#!/usr/bin/env python3
"""Copy C sources from the WaveDB monorepo into c_src/ for sdist packaging.

Run from the WaveDB repo root or from bindings/python/:
    python scripts/copy_sources.py

Modelled on bindings/nodejs/scripts/copy-sources.js. Produces a self-contained
c_src/ directory that setup.py can feed to CMake at pip-install time, without
needing the surrounding monorepo.
"""
from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path


def find_repo_root() -> Path:
    cwd = Path.cwd()
    while not (cwd / "src" / "HBTrie" / "hbtrie.h").exists():
        parent = cwd.parent
        if parent == cwd:
            print("Error: Cannot find WaveDB source tree.", file=sys.stderr)
            sys.exit(1)
        cwd = parent
    return cwd


def copy_file(src: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)


def copy_dir(src: Path, dest: Path, skip_dirs: set[str] | None = None) -> None:
    if skip_dirs is None:
        skip_dirs = set()
    dest.mkdir(parents=True, exist_ok=True)
    for entry in src.iterdir():
        if entry.name in skip_dirs:
            continue
        s = entry
        d = dest / entry.name
        if s.is_dir():
            copy_dir(s, d, skip_dirs)
        elif s.is_file():
            shutil.copy2(s, d)


def main() -> None:
    repo = find_repo_root()
    c_src = repo / "bindings" / "python" / "c_src"

    print(f"Copying C sources from {repo} to {c_src}")

    if c_src.exists():
        shutil.rmtree(c_src)
    c_src.mkdir(parents=True)

    # 1. WaveDB core sources
    copy_dir(repo / "src", c_src / "src")

    # 2. xxhash
    xxhash_dest = c_src / "deps" / "xxhash"
    xxhash_dest.mkdir(parents=True, exist_ok=True)
    for f in ["xxhash.c", "xxhash.h", "xxh3.h", "xxh_x86dispatch.c", "xxh_x86dispatch.h"]:
        s = repo / "deps" / "xxhash" / f
        if s.exists():
            copy_file(s, xxhash_dest / f)

    # 3. hashmap
    for sub in ["src", "include"]:
        d = c_src / "deps" / "hashmap" / sub
        d.mkdir(parents=True, exist_ok=True)
        s_dir = repo / "deps" / "hashmap" / sub
        if s_dir.exists():
            for f in s_dir.iterdir():
                if f.is_file():
                    copy_file(f, d / f.name)

    # 4. libcbor (full dir minus test/example, CMake needs CMakeLists.txt)
    copy_dir(repo / "deps" / "libcbor", c_src / "deps" / "libcbor",
             skip_dirs={"test", "example", "tests", "examples", ".git"})

    # 5. Root CMakeLists.txt
    copy_file(repo / "CMakeLists.txt", c_src / "CMakeLists.txt")

    # 6. cmake/ (WaveDBConfig.cmake.in — only for install export, harmless to include)
    cmake_src = repo / "cmake"
    if cmake_src.exists():
        copy_dir(cmake_src, c_src / "cmake")

    # Count files
    count = sum(1 for _ in c_src.rglob("*") if _.is_file())
    print(f"Done: {count} files copied to {c_src}")


if __name__ == "__main__":
    main()