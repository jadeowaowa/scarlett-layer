@echo off
setlocal

set "NEED_VCVARS=0"
where cl.exe >nul 2>&1
if errorlevel 1 set "NEED_VCVARS=1"
where cmake.exe >nul 2>&1
if errorlevel 1 set "NEED_VCVARS=1"

set "VS_INSTALL="
if "%NEED_VCVARS%"=="0" goto :find_cmake

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] cl.exe/cmake.exe not on PATH and vswhere.exe not found.
    exit /b 1
)

for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
if defined VS_INSTALL goto :have_vs
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"

:have_vs
if not defined VS_INSTALL (
    echo [ERROR] No Visual Studio installation with VC x64 tools found via vswhere.
    exit /b 1
)

call "%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%

:find_cmake
set "CMAKE_EXE=cmake"
where cmake.exe >nul 2>&1
if not errorlevel 1 goto :run_build

if not defined VS_INSTALL goto :no_cmake
if exist "%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE_EXE=%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if exist "%CMAKE_EXE%" goto :run_build

:no_cmake
echo [ERROR] cmake.exe not found on PATH or in Visual Studio installation.
exit /b 1

:run_build
rem Fresh clones have no build tree yet; configure it the same way CI does.
if exist "%~dp0..\build\CMakeCache.txt" goto :do_build
"%CMAKE_EXE%" -S "%~dp0.." -B "%~dp0..\build" -G Ninja -DCMAKE_BUILD_TYPE=Debug
if errorlevel 1 exit /b %errorlevel%

:do_build
"%CMAKE_EXE%" --build "%~dp0..\build" %*
exit /b %errorlevel%
