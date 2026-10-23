# Core/resource checks and native offline smoke; never uses formal account data.
param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release', [string]$BuildDir, [string]$ReportDir, [switch]$SkipBuild)
. (Join-Path $PSScriptRoot 'windows-common.ps1')
try {
    Assert-SzuWindows
    $context = Get-SzuContext
    if (-not $BuildDir) { $BuildDir = $context.DefaultBuildDir }
    $BuildDir = Get-SzuExternalDirectory $BuildDir $context.Repo
    if (-not $ReportDir) { $ReportDir = Join-Path $BuildDir 'reports' }
    $ReportDir = Get-SzuExternalDirectory $ReportDir $context.Repo
    $python = Get-SzuPython
    $version = Get-SzuVersion $context
    if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build-windows.ps1') -Configuration $Configuration -BuildDir $BuildDir }
    $exe = Get-SzuExecutable $BuildDir $Configuration $version
    $testRun = Join-Path $ReportDir ('ctest-' + $Configuration + '-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($testRun) | Out-Null
    Invoke-SzuNative 'ctest' @('--test-dir', $BuildDir, '-C', $Configuration, '--output-on-failure', '--output-junit', (Join-Path $testRun 'core-tests.xml')) 'Core tests'
    foreach ($tool in 'tools/reference/verify_fixtures.py', 'tools/generate_shared.py', 'tools/check_ui_resources.py') {
        $arguments = @($python.Prefix) + @((Join-Path $context.Repo $tool))
        if ($tool -eq 'tools/generate_shared.py') { $arguments += '--check' }
        Invoke-SzuNative $python.Program $arguments $tool
    }
    $smoke = & (Join-Path $PSScriptRoot 'self-check-windows.ps1') -Configuration $Configuration -BuildDir $BuildDir -ReportDir $ReportDir
    if ((Get-FileHash -LiteralPath $exe.Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $smoke.ExecutableSha256) { throw 'Executable changed after the smoke check.' }
    Write-Host 'Core, shared-resource and native offline checks passed.'
    $global:LASTEXITCODE = 0
    return $smoke
} catch { $global:LASTEXITCODE = 1; throw }
