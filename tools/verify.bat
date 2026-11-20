@echo off
call "%~dp0build.bat"
if errorlevel 1 exit /b %errorlevel%
ctest --test-dir "%~dp0..\build" --output-on-failure %*
exit /b %errorlevel%