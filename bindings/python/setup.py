import os
import shutil
import subprocess
import sys
from pathlib import Path

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
LIB_ENV = "WAVEDB_LIB_PATH"

if sys.platform == "darwin":
    SUFFIX = ".dylib"
elif sys.platform == "win32":
    SUFFIX = ".dll"
else:
    SUFFIX = ".so"


class CmakeBuildExt(build_ext):
    def get_ext_filename(self, ext_name: str) -> str:
        # Map the extension module name to a plain shared-lib filename
        # without the CPython ABI tag so cffi can dlopen("libwavedb.so").
        return ext_name.replace(".", os.sep) + SUFFIX

    def build_extension(self, ext: Extension) -> None:
        ext_path = Path(self.get_ext_fullpath(ext.name))
        lib_dir = ext_path.parent
        lib_dir.mkdir(parents=True, exist_ok=True)

        source_lib = os.environ.get(LIB_ENV)
        if source_lib:
            shutil.copy(source_lib, lib_dir / Path(source_lib).name)
            # Ensure a canonical libwavedb.<suffix> exists for cffi dlopen.
            canonical = lib_dir / f"libwavedb{SUFFIX}"
            if Path(source_lib).name != canonical.name and not canonical.exists():
                shutil.copy(source_lib, canonical)
            return

        if not shutil.which("cmake"):
            raise RuntimeError(
                "cmake not found on PATH. Install CMake (>=3.14) to build libwavedb, "
                "or set WAVEDB_LIB_PATH to point at a pre-built libwavedb.so."
            )

        build_dir = Path(self.build_temp) / "wavedb-shared"
        build_dir.mkdir(parents=True, exist_ok=True)
        subprocess.check_call([
            "cmake", "-S", str(REPO_ROOT), "-B", str(build_dir),
            "-DBUILD_PYTHON_BINDINGS=ON",
            "-DCMAKE_BUILD_TYPE=Release",
        ])
        subprocess.check_call([
            "cmake", "--build", str(build_dir), "--config", "Release",
            "--target", "wavedb_shared", "--", "-j", str(os.cpu_count() or 2),
        ])

        candidates = list(build_dir.glob(f"libwavedb*{SUFFIX}")) + list(build_dir.glob(f"wavedb*{SUFFIX}"))
        if not candidates:
            raise RuntimeError(f"libwavedb{SUFFIX} not found in {build_dir}")
        # Prefer the unversioned libwavedb.<suffix> over versioned symlinks
        # (libwavedb.so.0, libwavedb.so.0.1.0) for determinism.
        candidates.sort(key=lambda p: len(p.name))
        chosen = candidates[0]
        shutil.copy(chosen, ext_path)


setup(
    ext_modules=[Extension("wavedb._lib.libwavedb", sources=[])],
    cmdclass={"build_ext": CmakeBuildExt},
)
