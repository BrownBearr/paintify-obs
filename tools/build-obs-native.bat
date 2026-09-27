@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "REPO=%~dp0.."
set "NINJA=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
cmake -S "%REPO%\obs" -B "%REPO%\build-obs-ninja" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="%~3\scripts\buildsystems\vcpkg.cmake" -DOBS_SOURCE_DIR="%~1" -DOBS_LIBRARY="%~2"
if errorlevel 1 exit /b 1
cmake --build "%REPO%\build-obs-ninja"
if errorlevel 1 exit /b 1
