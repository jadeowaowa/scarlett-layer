@echo off
setlocal
call "%~dp0build.bat"
if errorlevel 1 exit /b %errorlevel%

set "CTEST_EXE=ctest"
where ctest >nul 2>&1
if errorlevel 1 (
    set "CTEST_EXE=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
)

"%CTEST_EXE%" --test-dir "%~dp0..\build" --output-on-failure %*
exit /b %errorlevel%