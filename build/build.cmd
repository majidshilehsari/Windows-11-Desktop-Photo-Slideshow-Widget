@echo off
rem  Build desktop-slideshow.exe locally on Windows (needs MSYS2 with the
rem  MinGW64 toolchain).  Installs what is missing via pacman if you say yes.
setlocal
where gcc >nul 2>nul || (
  echo No gcc found - trying to start an MSYS2 MinGW64 shell...
  for %%D in (C:\msys64 C:\msys32) do if exist "%%D\msys2_shell.cmd" (
    "%%D\msys2_shell.cmd" -mingw64 -defterm -here -c "pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-headers mingw-w64-ucrt-x86_64-winpthreads-static zip; cd /$OLDPWD; powershell -ExecutionPolicy Bypass -File build/build.ps1"
    goto :done
  )
  echo Install MSYS2 from https://www.msys2.org first, then re-run this file.
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
:done
