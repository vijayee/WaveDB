import os
import shutil
import subprocess
import sys
from pathlib import Path

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext


# Source tree locator: prefer vendored c_src/ (populated by scripts/copy_sources.py
# at publish time, included in sdists). Fall back to the monorepo root for dev
# checkouts. This makes pip-install from an extracted sdist work without the
# surrounding repo.
HERE = Path(__file__).resolve().parent
VENDOR = HERE / "c_src"
REPO_ROOT = VENDOR if (VENDOR / "CMakeLists.txt").exists() else HERE.parent.parent.parent
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

        # ---- Step 1: Get libwavedb.so into lib_dir ----

        source_lib = os.environ.get(LIB_ENV)
        if source_lib:
            shutil.copy(source_lib, lib_dir / Path(source_lib).name)
            canonical = lib_dir / f"libwavedb{SUFFIX}"
            if Path(source_lib).name != canonical.name and not canonical.exists():
                shutil.copy(source_lib, canonical)
        elif (REPO_ROOT / "CMakeLists.txt").exists() and shutil.which("cmake"):
            build_dir = Path(self.build_temp) / "wavedb-shared"
            build_dir.mkdir(parents=True, exist_ok=True)
            subprocess.check_call([
                "cmake", "-S", str(REPO_ROOT), "-B", str(build_dir),
                "-DBUILD_PYTHON_BINDINGS=ON",
                "-DBUILD_TESTS=OFF",
                "-DCMAKE_BUILD_TYPE=Release",
            ])
            # Parallel build flag: make uses "-j N"; MSBuild (Windows / VS
            # generator) uses "/m:N". cmake forwards everything after "--"
            # to the underlying build tool verbatim.
            if sys.platform == "win32":
                parallel_args = [f"/m:{os.cpu_count() or 2}"]
            else:
                parallel_args = ["-j", str(os.cpu_count() or 2)]
            subprocess.check_call([
                "cmake", "--build", str(build_dir), "--config", "Release",
                "--target", "wavedb_shared", "--", *parallel_args,
            ])

            candidates = list(build_dir.glob(f"libwavedb*{SUFFIX}")) + list(build_dir.glob(f"wavedb*{SUFFIX}"))
            if not candidates:
                raise RuntimeError(f"libwavedb{SUFFIX} not found in {build_dir}")
            candidates.sort(key=lambda p: len(p.name))
            shutil.copy(candidates[0], ext_path)
        else:
            # Fall back to bundled .so
            bundled = HERE / "src" / "wavedb" / "_lib" / f"libwavedb{SUFFIX}"
            if not bundled.exists():
                raise RuntimeError(
                    f"libwavedb{SUFFIX} not found and no C source tree to build from. "
                    f"Set WAVEDB_LIB_PATH to point at a pre-built libwavedb{SUFFIX}."
                )
            shutil.copy(bundled, ext_path)

        # ---- Step 2: Compile cffi out-of-line extension ----
        # Links against libwavedb.so in lib_dir. Rpath ($ORIGIN/_lib or
        # @loader_path/_lib) ensures the extension finds the .so at runtime.
        try:
            src_dir = HERE / "src"
            sys.path.insert(0, str(src_dir))
            from wavedb._cffi_build import ffibuilder, configure
            # Use absolute path for library_dir so the linker finds libwavedb.so
            configure(library_dir=str(lib_dir.resolve()))
            cffi_tmp = Path(self.build_temp) / "cffi"
            cffi_tmp.mkdir(parents=True, exist_ok=True)
            compiled_path = ffibuilder.compile(tmpdir=str(cffi_tmp), verbose=True)
            # Copy the compiled extension to the package directory
            if compiled_path:
                shutil.copy(compiled_path, lib_dir)
        except Exception as e:
            # If cffi compilation fails, the binding falls back to ABI mode
            # (_native.py tries _native_ext, then _native_abi on ImportError).
            print(f"Warning: cffi out-of-line compilation failed ({e}); "
                  f"falling back to ABI mode")


setup(
    ext_modules=[Extension("wavedb._lib.libwavedb", sources=[])],
    cmdclass={"build_ext": CmakeBuildExt},
)