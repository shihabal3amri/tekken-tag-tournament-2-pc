@echo off
setlocal
for %%I in ("%~dp0..") do set "TTT2_WORKSPACE=%%~fI"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
"%CMAKE%" -S "%TTT2_WORKSPACE%\port\src\launcher" -B "%TTT2_WORKSPACE%\port\build-launcher" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
"%CMAKE%" --build "%TTT2_WORKSPACE%\port\build-launcher" --parallel 4
