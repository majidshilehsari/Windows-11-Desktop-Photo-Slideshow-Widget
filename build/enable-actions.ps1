<#
  Installs build/build-windows.yml as .github/workflows/build-windows.yml in your
  GitHub repository, so the .exe can be built by GitHub Actions.  Needed because a
  GitHub-App token (what a bot / coding agent uses) may not push files under
  .github/workflows.

      powershell -ExecutionPolicy Bypass -File build\enable-actions.ps1 [-Branch main]

  Requirements: git + GitHub CLI, with  gh auth login  done as YOURSELF
  (check with  gh auth status  - it must not be a bot account).
#>
[CmdletBinding()]
param(
    [string]$Owner = '',
    [string]$Repo  = '',
    [string]$Branch = 'main',
    [string]$Source  = 'build/build-windows.yml'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Write-Host "GitHub CLI not found. Install it with:  winget install --id GitHub.cli" -ForegroundColor Red
    Write-Host "Then run: gh auth login   (HTTPS + browser flow)" -ForegroundColor Yellow
    exit 2
}
$me = (gh api user --jq .login) 2>$null
if (-not $me) { Write-Host "gh is not authenticated as a person - run:  gh auth login" -ForegroundColor Red; exit 2 }
Write-Host "using GitHub account: $me" -ForegroundColor Cyan

if (-not $Owner -or -not $Repo) {
    $remote = git remote get-url origin
    if ($remote -match '[:/]([^/]+)/([^/]+?)(\.git)?/?$') { $Owner = $Matches[1]; $Repo = $Matches[2] }
    else { Write-Host "Cannot read owner/repo from the 'origin' remote - pass -Owner/-Repo" -ForegroundColor Red; exit 2 }
}
$src = Join-Path $root $Source
if (-not (Test-Path $src)) { throw "missing $src" }
$b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($src))
$path = '.github/workflows/build-windows.yml'

$body = @{ message = 'Add GitHub Actions workflow: build the Windows exe'; content = $b64; branch = $Branch } |
        ConvertTo-Json -Compress
try {
    $body | gh api -X PUT "repos/$Owner/$Repo/contents/$path" --input - | Out-Null
    Write-Host "installed $path in $Owner/$Repo on branch $Branch" -ForegroundColor Green
} catch {
    Write-Host "Could not create the file: $_" -ForegroundColor Red
    Write-Host "Fallback: on github.com -> $Owner/$Repo -> Add file -> Create new file -> name it $path -> paste the contents of $Source -> Commit." -ForegroundColor Yellow
    exit 3
}
Write-Host ''
Write-Host 'Next:' -ForegroundColor Cyan
Write-Host "  1. Actions tab -> 'Build Windows exe' -> Run workflow (branch: $Branch) -> download the artefact"
Write-Host  '  2. or publish:  git tag v1.0.0 ; git push origin v1.0.0   -> the exe shows up under Releases'
