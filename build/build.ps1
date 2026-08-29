این بار گناهکار build.ps1 بود، نه کامپایل. دقیقاً این خط را نوشته بودم:

($_ -replace '\','/')      # ← '\' در PowerShell یک ریگولار اکسپرشن است و نامعتبر
ویندوز به‌درستی می‌گوید The regular expression pattern \ is not valid. جای Replace متن ساده، -replace (ریگولار) را گذاشته بودم. سه اصلاح انجام شد:

-replace '\' → تابع Fwd با .Replace('\','/') (جایگزینی متنی، بدون ریگولار).
-lshcore و -lwindowscodecs از لینک حذف شد؛ GetDpiForMonitor حالا در رانتایم از SHCORE.dll با LoadLibraryEx گرفته می‌شود و fallback به GetDeviceCaps دارد → دیگر به بودن/نبودن import library در SDK بستگی ندارد (کد src/app.cpp هم همین را پیاده می‌کند).
response-file با مسیر نسبی dist/build.rsp به g++ داده می‌شود و توکن‌های -D بدون کوتیشن نوشته می‌شوند (قبلاً -DDSKV_VERSION_STR=L"..." داخل کوتیشن خراب می‌شد).
کاری که می‌کنی
workflow را دست نزن (همان install: mingw-w64-x86_64-toolchain که فرستادی درست است). فقط build/build.ps1 را کامل عوض کن:

https://github.com/majidshilehsari/Windows-11-Desktop-Photo-Slideshow-Widget/edit/main/build/build.ps1 → همه را پاک کن → این را paste کن → Commit → Actions → Build Windows exe → Run workflow.

<#
  build.ps1 - builds desktop-slideshow.exe with a MinGW-w64 toolchain (g++).
  Used both by GitHub Actions and locally (build\build.cmd).
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
    Write-Host "== $File $($Arguments -join ' ')" -ForegroundColor Cyan
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}

function Fwd([string]$p) { return $p.Replace('\', '/') }   # literal, not regex

foreach ($tool in @('g++', 'windres')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool not found. Use an MSYS2 MinGW64 shell, or run build\build.cmd."
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
$rcGen = Join-Path (Join-Path $root 'src') 'app-generated.rc'
Set-Content -Path $rcGen -Value $rcSrc -Encoding ASCII

$res = Join-Path $out 'app.res'
Invoke-Step 'windres' @('-i', (Fwd $rcGen), '-O', 'coff', '-o', (Fwd $res))
Remove-Item $rcGen -Force

$exe = Join-Path $out "$JobName.exe"

# ---- compile + link through a response file (no quoting hazards) ----
$defines = @('-DUNICODE','-D_UNICODE','-DDSKV_VERSION_STR=L"' + ($parts -join '.') + '"')
$cxxFlags = @('-std=c++17','-O2','-s','-Wall','-Wextra','-Wno-unused-parameter',
              '-municode','-mwindows','-Wl,--subsystem,windows')
$linkFlags = @('-static','-static-libgcc','-static-libstdc++')
$libs = @('-lgdiplus','-lcomctl32','-lshell32','-lwtsapi32',
          '-luser32','-lgdi32','-lole32','-loleaut32','-luuid','-lkernel32','-ladvapi32')
if ($ExtraFlags) { $cxxFlags += (@($ExtraFlags -split '\s+') | Where-Object { $_ }) }

function Write-Rsp([string]$path, [string[]]$lines) {
    $lines | ForEach-Object {
        $t = Fwd $_
        if ($t.StartsWith('-D')) { $t } else { '"' + $t + '"' }
    } | Set-Content -Encoding ascii -Path $path
}

$rspRel = (Fwd (Join-Path $OutDir 'build.rsp'))
$rsp = Join-Path $out 'build.rsp'
Write-Rsp $rsp ($cxxFlags + $linkFlags + $defines + $srcFiles + @($res, '-o', $exe) + $libs)

$shippedRuntime = $false
try {
    Invoke-Step 'g++' @('@' + $rspRel)
} catch {
    # no static libwinpthread/libstdc++ in this MinGW? link dynamically and ship the DLLs
    Write-Host "static link failed - retrying with a dynamic runtime" -ForegroundColor Yellow
    Write-Rsp $rsp ($cxxFlags + $defines + $srcFiles + @($res, '-o', $exe) + $libs)
    Invoke-Step 'g++' @('@' + $rspRel)
    $shippedRuntime = $true
}

if (Get-Command upx -ErrorAction SilentlyContinue) {
    Write-Host '== upx' -ForegroundColor Cyan
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
Write-Host ('built {0} ({1:N0} bytes)' -f (Split-Path $exe -Leaf), (Get-Item $exe).Length) -ForegroundColor Green
Write-Host ('built {0}' -f $zip) -ForegroundColor Green
