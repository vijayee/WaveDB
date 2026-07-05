@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "C:\Users\victor morrow\Git-projects\WaveDB"
REM Build a Windows wheel from the local repo. setup.py auto-discovers the
REM pre-built wavedb.dll under c_src/build-msvc/ and bundles it, so the
REM wheel installs without CMake/MSVC on the target machine.
python -m pip wheel "bindings\python" -w dist --no-build-isolation --no-deps
echo WHEEL_EXIT=%ERRORLEVEL%