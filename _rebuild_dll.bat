@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "C:\Users\victor morrow\Git-projects\WaveDB"
cmake --build build-msvc12 --config Release --target wavedb_shared
echo BUILD_EXIT=%ERRORLEVEL%