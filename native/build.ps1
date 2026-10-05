<#
.SYNOPSIS
  Build, test and package the native ClaudeUsageTracker Win32 app.

.DESCRIPTION
  Locates cmake (PATH, C:\CMake\bin, or the VS-bundled copy via vswhere),
  configures only when build\CMakeCache.txt is missing, builds, runs ctest,
  and copies the exe to native\dist\ClaudeUsageTracker.exe.

.PARAMETER Config  Release (default) or Debug.
.PARAMETER Clean   Move native\build to native\.trash\build-<timestamp> (not deleted) first.
.PARAMETER NoTests Skip ctest.
.PARAMETER Run     Start the exe after a successful build.

.EXAMPLE
  build.cmd                    # Release build + tests
  build.cmd -Clean -NoTests    # fresh configure, skip tests
  build.cmd -Config Debug -Run
#>
param(
    [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
    [switch]$Clean,
    [switch]$NoTests,
    [switch]$Run
)
$ErrorActionPreference = 'Stop'

$root     = $PSScriptRoot
$buildDir = Join-Path $root 'build'
$distDir  = Join-Path $root 'dist'

function Find-CMake {
    $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if (Test-Path 'C:\CMake\bin\cmake.exe') { return 'C:\CMake\bin\cmake.exe' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath
        if ($vs) {
            $p = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $p) { return $p }
        }
    }
    throw 'cmake not found (PATH, C:\CMake\bin, or Visual Studio bundled).'
}

function Invoke-Step([string]$What, [scriptblock]$Cmd) {
    Write-Host "==> $What"
    & $Cmd
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit code $LASTEXITCODE)" }
}

try {
    $cmake = Find-CMake
    $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
    Write-Host "cmake: $cmake"

    if ($Clean -and (Test-Path $buildDir)) {
        $trash = Join-Path $root '.trash'
        New-Item -ItemType Directory -Force -Path $trash | Out-Null
        $dest = Join-Path $trash ('build-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        Move-Item -LiteralPath $buildDir -Destination $dest
        Write-Host "Moved old build to $dest"
    }

    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        Invoke-Step 'Configure' { & $cmake -S $root -B $buildDir -G 'Visual Studio 17 2022' -A x64 }
    } else {
        Write-Host '==> Configure skipped (CMakeCache.txt exists)'
    }

    Invoke-Step 'Build' { & $cmake --build $buildDir --config $Config }

    if (-not $NoTests) {
        Invoke-Step 'Test' { & $ctest --test-dir $buildDir -C $Config --output-on-failure }
    }

    $exe = Get-ChildItem -Path $buildDir -Recurse -Filter 'ClaudeUsageTracker.exe' |
           Where-Object { $_.Directory.Name -eq $Config } | Select-Object -First 1
    if (-not $exe) { throw "ClaudeUsageTracker.exe not found in build output for $Config" }

    New-Item -ItemType Directory -Force -Path $distDir | Out-Null
    $out = Join-Path $distDir 'ClaudeUsageTracker.exe'
    Copy-Item -LiteralPath $exe.FullName -Destination $out -Force
    Write-Host ("Output: {0} ({1:N0} bytes)" -f $out, (Get-Item $out).Length)

    if ($Run) { Start-Process -FilePath $out }
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
