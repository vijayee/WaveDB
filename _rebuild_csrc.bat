@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "C:\Users\victor morrow\Git-projects\WaveDB"
cmake -S bindings/python/c_src -B bindings/python/c_src/build-msvc -DBUILD_PYTHON_BINDINGS=ON -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
echo CONFIG_EXIT=%ERRORLEVEL%
cmake --build bindings/python/c_src/build-msvc --config Release --target wavedb_shared
echo BUILD_EXIT=%ERRORLEVEL%