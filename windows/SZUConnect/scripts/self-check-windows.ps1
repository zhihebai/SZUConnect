# Native offline receipt check. Each run gets a new report directory.
param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release', [string]$BuildDir, [string]$ReportDir)
. (Join-Path $PSScriptRoot 'windows-common.ps1')
$process = $null
$started = $false
$stdoutTask = $stderrTask = $null
try {
    Assert-SzuWindows
    $context = Get-SzuContext
    if (-not $BuildDir) { $BuildDir = $context.DefaultBuildDir }
    $BuildDir = Get-SzuExternalDirectory $BuildDir $context.Repo
    if (-not $ReportDir) { $ReportDir = Join-Path $BuildDir 'reports' }
    $ReportDir = Get-SzuExternalDirectory $ReportDir $context.Repo
    $version = Get-SzuVersion $context
    $exe = Get-SzuExecutable $BuildDir $Configuration $version
    $run = Join-Path $ReportDir ('smoke-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($run) | Out-Null
    Write-Host "Native smoke report directory: $run"
    $receiptPath = Join-Path $run 'receipt.json'
    if (Test-Path -LiteralPath $receiptPath) { throw 'Smoke receipt already exists; it will not be overwritten.' }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo.FileName = $exe.Path
    $process.StartInfo.Arguments = '--offline-smoke --smoke-output "' + $receiptPath + '" --smoke-timeout-ms 10000'
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $process.StartInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    $started = $process.Start()
    if (-not $started) { throw 'Native smoke process did not start.' }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(15000)) {
        $process.Kill()
        $null = $process.WaitForExit(5000)
        throw 'Native smoke exceeded the 15-second outer deadline. Logs are retained in the report directory.'
    }
    if ($process.ExitCode -ne 0) { throw "Native smoke failed (exit $($process.ExitCode)). Logs are retained in the report directory." }
    if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) { throw 'Native smoke did not write its receipt.' }
    $receipt = (Read-SzuUtf8 $receiptPath) | ConvertFrom-Json
    Assert-SzuReceipt $receipt $version
    $after = (Get-FileHash -LiteralPath $exe.Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($after -ne $exe.Sha256) { throw 'Executable changed during the smoke check.' }
    Write-Host "Native offline smoke passed: $receiptPath"
    $global:LASTEXITCODE = 0
    return [pscustomobject]@{ ReceiptPath = $receiptPath; ExecutableSha256 = $exe.Sha256; ReportDirectory = $run }
} catch { $global:LASTEXITCODE = 1; throw }
finally {
    if ($process) {
        if ($started -and -not $process.HasExited) { try { $process.Kill(); $null = $process.WaitForExit(5000) } catch {} }
        if ($stdoutTask -and $stdoutTask.Wait(5000)) { Write-SzuUtf8 (Join-Path $run 'stdout.log') $stdoutTask.Result }
        if ($stderrTask -and $stderrTask.Wait(5000)) { Write-SzuUtf8 (Join-Path $run 'stderr.log') $stderrTask.Result }
        $process.Dispose()
    }
}
