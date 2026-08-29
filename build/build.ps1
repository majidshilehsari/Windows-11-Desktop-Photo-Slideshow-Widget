<#
  build.ps1 - builds desktop-slideshow.exe with a MinGW-w64 toolchain.

  Works in a plain "MSYS2 MinGW64" shell on your own Windows 11 box:
      powershell -ExecutionPolicy Bypass -File build\build.ps1
  and in CI (see .github/workflows/build-windows.yml).
#>
[CmdletBinding()]
param(
    [string]$Version   = '0.1.0',
    [string]$OutDir    = 'dist',
    [string]$JobName   = 'desktop-slideshow',
    [string]$ExtraFlags = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

$srcFiles = @('main.cpp','app.cpp','config.cpp','images.cpp','render.cpp','cli.cpp') | ForEach-Object { "src\$_" }
$out = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Invoke-Step {
    param([string]$File, [string[]]$Arguments)
    Write-Host "== $File $($Arguments -join ' ')" -ForegroundColor Cyan
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}

if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    throw "gcc not found. Open an 'MSYS2 MinGW64' shell (or run the GitHub Actions workflow)."
}
& gcc --version | Select-Object -First 1

# ---- version info: patch the .rc, compile it (with the manifest) into a .res ----
$rcTmp = Join-Path ([IO.Path]::GetTempPath()) "dskv-$PID-app.rc"
$rcSrc = Get-Content -Raw 'src\app.rc'
$parts = ($Version -split '\.') + @('0','0','0')
$rcSrc = $rcSrc.Replace('0,1,0,0', (($parts[0..3] -join ',')))
$rcSrc = $rcSrc.Replace('"0.1.0.0"', ('"' + ($parts[0..3] -join '.') + '"'))
# windres resolves "app.ico" / "app.manifest" relative to the .rc location -> keep it in src\
$rcTmp = Join-Path (Join-Path $root 'src') "app-generated.rc"
Set-Content -Path $rcTmp -Value $rcSrc -Encoding ASCII

$res = Join-Path $out 'app.res'
Invoke-Step 'windres' @('-i', $rcTmp, '-O', 'coff', '-o', $res)
Remove-Item $rcTmp -Force

$exe = Join-Path $out "$JobName.exe"

# ---- build everything through a response file (avoids all quoting pain) ----
$defines = @(
    '-DUNICODE','-D_UNICODE',
    '-DDSKV_VERSION_STR=L"' + $Version + '"'
)
$flags = @(
    '-std=c++17','-O2','-s','-municode','-mwindows','-Wl,--subsystem,windows',
    '-Wall','-Wextra','-Wno-unused-parameter',
    '-static','-static-libgcc','-static-libstdc++'
)
if ($ExtraFlags) { $flags += ($ExtraFlags -split '\s+') | Where-Object { $_ } }
$libs = @('-lgdiplus','-lwindowscodecs','-lcomctl32','-lshell32','-lshcore',
          '-lwtsapi32','-luser32','-lgdi32','-lole32','-loleaut32','-luuid',
          '-lkernel32','-ladvapi32')
$rsp = Join-Path $out 'build.rsp'
@( $flags + $defines + $srcFiles + @($res, '-o', ($exe -replace '\','/')) + $libs ) |
    ForEach-Object { '"' + ($_ -replace '\','/') + '"' } | Set-Content -Encoding ascii $rsp

Invoke-Step 'g++' @('@' + ($rsp -replace '\','/'))

# ---- optional: pack it even smaller ----
if (Get-Command upx -ErrorAction SilentlyContinue) {
    Write-Host '== upx' -ForegroundColor Cyan
    & upx -9 --best $exe | Out-Null
}

# ---- portable zip with a ready-to-edit settings file ----
$stage = Join-Path $out 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $exe $stage
if (Test-Path 'slideshow.default.ini') { Copy-Item 'slideshow.default.ini' (Join-Path $stage 'slideshow.ini') }
foreach ($doc in @('README.md','LICENSE')) { if (Test-Path $doc) { Copy-Item $doc $stage } }
$zip = Join-Path $out "$JobName-$Version-win-x64-portable.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
(Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() + "  " + (Split-Path $zip -Leaf) |
    Set-Content -Encoding ascii (Join-Path $out "$JobName-$Version-SHA256.txt")

$size = (Get-Item $exe).Length
Write-Host ''
Write-Host ("built {0} ({1:N0} bytes)" -f (Split-Path $exe -Leaf), $size) -ForegroundColor Green
Write-Host ("built {0}" -f $zip) -ForegroundColor Green
