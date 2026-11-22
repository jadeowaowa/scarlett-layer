@echo off
setlocal
call "%~dp0build.bat"
if errorlevel 1 exit /b %errorlevel%

set "CTEST_EXE=ctest"
where ctest.exe >nul 2>&1
if not errorlevel 1 goto :run_ctest

set "VS_INSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] ctest.exe not on PATH and vswhere.exe not found.
    exit /b 1
)

for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
if defined VS_INSTALL goto :have_vs
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"

:have_vs
if not defined VS_INSTALL goto :no_ctest
if exist "%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" set "CTEST_EXE=%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
if exist "%CTEST_EXE%" goto :run_ctest

:no_ctest
echo [ERROR] ctest.exe not found on PATH or in Visual Studio installation.
exit /b 1

:run_ctest
"%CTEST_EXE%" --test-dir "%~dp0..\build" --output-on-failure %*
exit /b %errorlevel%
