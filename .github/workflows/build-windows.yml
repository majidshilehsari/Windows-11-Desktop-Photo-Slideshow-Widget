<#
  build.ps1 - builds desktop-slideshow.exe with a MinGW-w64 toolchain (g++).

  On your own Windows box, from an "MSYS2 MinGW64" shell (or just run build\build.cmd):
      powershell -ExecutionPolicy Bypass -File build\build.ps1
  It is also exactly what the GitHub Actions workflow runs, so both paths agree.

  IMPORTANT: keep this file pure ASCII and English-only.  Windows PowerShell 5.1
  reads a BOM-less .ps1 as ANSI, so UTF-8 comments in any other language arrive at
  the parser as mojibake and produce nonsense errors such as "Missing closing ')'".
  Non-ASCII text belongs in the .cpp sources (gcc reads UTF-8), never in the scripts.
#>
[CmdletBinding()]
param(
    [string]$Version    = '0.1.0',
    [string]$OutDir     = 'dist',
    [string]$JobName    = 'desktop-slideshow',
    [string]$ExtraFlags = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

function Invoke-Step {
    param([string]$File, [string[]]$Arguments)
    Write-Host "== $File $($Arguments -join ' ')"
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}

foreach ($tool in @('g++', 'windres')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool not found. Use an MSYS2 MinGW64 shell, or run build\build.cmd (it installs gcc/binutils/crt via pacman)."
    }
}
& g++ --version | Select-Object -First 1

$srcFiles = @('main.cpp','app.cpp','config.cpp','images.cpp','render.cpp','cli.cpp') |
            ForEach-Object { "src/$_" }

$out = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $out | Out-Null

# ---- version info: patch the .rc, then compile it (with the manifest) to .res ----
$parts = (@($Version -split '\.') + @('0','0','0','0'))[0..3]
$rcSrc = Get-Content -Raw 'src/app.rc'
$rcSrc = $rcSrc.Replace('0,1,0,0', ($parts -join ','))
$rcSrc = $rcSrc.Replace('"0.1.0.0"', '"' + ($parts -join '.') + '"')
# windres resolves "app.ico"/"app.manifest" relative to the .rc file, so keep it inside src/
$rcGen = Join-Path (Join-Path $root 'src') 'app-generated.rc'
Set-Content -Path $rcGen -Value $rcSrc -Encoding ASCII

$res = Join-Path $out 'app.res'
Invoke-Step 'windres' @('-i', (Join-Path $root 'src\app-generated.rc'), '-O', 'coff',
                        '-o', (Join-Path $out 'app.res'))
Remove-Item $rcGen -Force

$exe = Join-Path $out "$JobName.exe"

# ---- compile + link ----
# The version macro holds embedded double quotes, so g++ gets a real argument array
# (PowerShell does no shell parsing for native commands); dist/build.rsp is written
# as well, only so that a failing CI log can be compared against the exact flags.
$defines = @('-DUNICODE','-D_UNICODE','-DDSKV_VERSION_STR=L"' + ($parts -join '.') + '"')
$cxxFlags = @('-std=c++17','-O2','-s','-Wall','-Wextra','-Wno-unused-parameter',
              '-municode','-mwindows','-Wl,--subsystem,windows')
$linkFlags = @('-static','-static-libgcc','-static-libstdc++')
$libs = @('-lgdiplus','-lcomctl32','-lshell32','-lwtsapi32',
          '-luser32','-lgdi32','-lole32','-loleaut32','-luuid','-lkernel32','-ladvapi32')
if ($ExtraFlags) { $cxxFlags += (@($ExtraFlags -split '\s+') | Where-Object { $_ }) }

function Fwd([string]$p) { return $p.Replace('\', '/') }   # literal replace; -replace would compile a regex
$($cxxFlags + $linkFlags + $defines + $srcFiles + @($res, '-o', $exe) + $libs) |
    ForEach-Object { if ($_.StartsWith('-D')) { $_ } else { '"' + (Fwd $_) + '"' } } |
    Set-Content -Encoding ascii -Path (Join-Path $out 'build.rsp')

$gppArgs = $cxxFlags + $linkFlags + $defines + $srcFiles + @($res, '-o', $exe) + $libs

$shippedRuntime = $false
try {
    Invoke-Step 'g++' $gppArgs
} catch {
    # Some MinGW-w64 builds ship no static libwinpthread.a / libstdc++.a.  Rather
    # than failing, link against the DLL runtime and ship those DLLs next to the exe.
    Write-Host "static link failed - retrying with a dynamic runtime"
    Invoke-Step 'g++' ($cxxFlags + $defines + $srcFiles + @($res, '-o', $exe) + $libs)
    $shippedRuntime = $true
}

# ---- optional: pack it even smaller (may upset SmartScreen / AV) ----
if (Get-Command upx -ErrorAction SilentlyContinue) {
    Write-Host '== upx'
    & upx -9 --best $exe | Out-Null
}

# ---- portable zip with a ready-to-edit settings file ----
$stage = Join-Path $out 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $exe $stage
if ($shippedRuntime) {
    foreach ($dll in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
        $p = (Get-Command $dll -ErrorAction SilentlyContinue).Source
        if ($p) { Copy-Item $p $stage }
    }
}
if (Test-Path 'slideshow.default.ini') { Copy-Item 'slideshow.default.ini' (Join-Path $stage 'slideshow.ini') }
foreach ($doc in @('README.md','LICENSE')) { if (Test-Path $doc) { Copy-Item $doc $stage } }

$zip = Join-Path $out "$JobName-$($parts -join '.')-win-x64-portable.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
(Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() + '  ' + (Split-Path $zip -Leaf) |
    Set-Content -Encoding ascii (Join-Path $out "$JobName-$($parts -join '.')-SHA256.txt")

Write-Host ''
Write-Host ('built {0} ({1:N0} bytes)' -f (Split-Path $exe -Leaf), (Get-Item $exe).Length)
Write-Host ('built {0}' -f $zip)
