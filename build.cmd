@echo off
rem  build.cmd - convenience wrapper for build\build.cmd (build desktop-slideshow.exe locally)
@powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0builduild.ps1" %*
