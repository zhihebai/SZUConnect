# Build only: VS 2022/MSVC v143 + Windows SDK + CMake 3.21 or newer.
param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release', [string]$BuildDir)
. (Join-Path $PSScriptRoot 'windows-common.ps1')
try {
    Assert-SzuWindows
    $context = Get-SzuContext
    if (-not $BuildDir) { $BuildDir = $context.DefaultBuildDir }
    $BuildDir = Get-SzuExternalDirectory $BuildDir $context.Repo
    $version = Get-SzuVersion $context
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'CMake 3.21+ was not found. Install VS 2022 Desktop development with C++, MSVC v143 and Windows SDK, then open Developer PowerShell.' }
    Invoke-SzuNative 'cmake' @('-S', $context.Project, '-B', $BuildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64', '-DBUILD_TESTING=ON') 'Configure (VS 2022 C++ workload and Windows SDK required)'
    Invoke-SzuNative 'cmake' @('--build', $BuildDir, '--config', $Configuration) 'Build'
    $exe = Get-SzuExecutable $BuildDir $Configuration $version
    Write-Host "Built and version-checked: $($exe.Path) [$($version.Numeric)]"
    $global:LASTEXITCODE = 0
} catch { $global:LASTEXITCODE = 1; throw }
