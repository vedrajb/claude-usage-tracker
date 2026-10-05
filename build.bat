@echo off
rem Builds the native Claude Usage Tracker (wrapper for native\build.ps1).
rem Usage: build.bat [-Config Release^|Debug] [-Clean] [-NoTests] [-Run]
rem Output: native\dist\ClaudeUsageTracker.exe
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0native\build.ps1" %*
exit /b %ERRORLEVEL%
