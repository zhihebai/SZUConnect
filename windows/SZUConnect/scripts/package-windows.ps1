# Release package only. Existing ZIP/hash/receipt files are never replaced.
param([string]$BuildDir, [string]$ReportDir, [string]$OutputDir, [switch]$SkipBuild)
. (Join-Path $PSScriptRoot 'windows-common.ps1')
$stageRoot = $null
$lock = $null
$published = [Collections.Generic.List[string]]::new()
try {
    Assert-SzuWindows
    $context = Get-SzuContext
    if (-not $BuildDir) { $BuildDir = $context.DefaultBuildDir }
    $BuildDir = Get-SzuExternalDirectory $BuildDir $context.Repo
    if (-not $ReportDir) { $ReportDir = Join-Path $BuildDir 'reports' }
    $ReportDir = Get-SzuExternalDirectory $ReportDir $context.Repo
    if (-not $OutputDir) { $OutputDir = Join-Path $BuildDir 'dist' }
    $OutputDir = Get-SzuExternalDirectory $OutputDir $context.Repo
    $version = Get-SzuVersion $context
    $name = "SZUConnect-$($version.Version)-$($version.Build)-win-x64"
    $zip = Join-Path $OutputDir "$name.zip"
    $hashPath = $zip + '.sha256'
    $resultPath = Join-Path $OutputDir "$name.package.json"
    $standalone = Join-Path $OutputDir "$name.exe"
    $standaloneHash = $standalone + '.sha256'
    foreach ($path in @($zip, $hashPath, $resultPath, $standalone, $standaloneHash)) {
        if (Test-Path -LiteralPath $path) { throw 'A package output already exists. Choose a new OutputDir; existing artifacts are retained.' }
    }
    [IO.Directory]::CreateDirectory($OutputDir) | Out-Null
    $lockPath = Join-Path $OutputDir ('.' + $name + '.lock')
    $lock = [IO.File]::Open($lockPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $python = Get-SzuPython
    $smoke = & (Join-Path $PSScriptRoot 'test-windows.ps1') -Configuration Release -BuildDir $BuildDir -ReportDir $ReportDir -SkipBuild:$SkipBuild
    $exe = Get-SzuExecutable $BuildDir 'Release' $version
    if ($exe.Sha256 -ne $smoke.ExecutableSha256) { throw 'Executable changed after testing.' }
    $stageRoot = Join-Path $OutputDir ('.szu-package-' + [Guid]::NewGuid().ToString('N'))
    $stage = Join-Path $stageRoot $name
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    [IO.File]::Copy($exe.Path, (Join-Path $stage 'SZUConnect.exe'), $false)
    # Keep the script ASCII-compatible with Windows PowerShell 5.1.
    $infoName = -join @([char]0x8BF4, [char]0x660E, [char]0x4E0E, [char]0x8BB8, [char]0x53EF)
    $info = Join-Path $stage $infoName
    [IO.Directory]::CreateDirectory($info) | Out-Null
    [IO.File]::Copy((Join-Path $context.Repo 'docs/PACKAGE-WINDOWS.md'), (Join-Path $info 'README.md'), $false)
    [IO.File]::Copy((Join-Path $context.Repo 'LICENSE'), (Join-Path $info 'LICENSE'), $false)
    [IO.File]::Copy((Join-Path $context.Repo 'docs/DISTRIBUTION-NOTICES.txt'), (Join-Path $info 'THIRD-PARTY-NOTICES.md'), $false)
    [IO.File]::Copy((Join-Path $context.Repo 'docs/licenses/curl-COPYING.txt'), (Join-Path $info 'curl-COPYING.txt'), $false)
    [IO.File]::Copy($smoke.ReceiptPath, (Join-Path $info 'WINDOWS-SMOKE.json'), $false)
    $files = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Force | ForEach-Object {
        $relative = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [ordered]@{ name = $relative; size = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    } | Sort-Object { $_.name })
    if (($files | Where-Object { $_.name -eq 'SZUConnect.exe' }).sha256 -ne $exe.Sha256) { throw 'Staged executable bytes differ from the tested executable.' }
    $manifest = [ordered]@{ schemaVersion = 1; version = $version.Version; build = $version.Build; architecture = 'x64'; configuration = 'Release'; signingPerformedByScript = $false; files = $files }
    Write-SzuUtf8 (Join-Path $info 'PACKAGE-MANIFEST.json') ($manifest | ConvertTo-Json -Depth 10)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $temporaryZip = Join-Path $stageRoot 'candidate.zip'
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $temporaryZip, [IO.Compression.CompressionLevel]::Optimal, $true)
    Invoke-SzuNative $python.Program (@($python.Prefix) + @((Join-Path $PSScriptRoot 'verify-package.py'), '--archive', $temporaryZip, '--expected-root', $name)) 'ZIP inventory, CRC and hash verification'
    $hash = (Get-FileHash -LiteralPath $temporaryZip -Algorithm SHA256).Hash.ToLowerInvariant()
    $temporaryHash = Join-Path $stageRoot 'candidate.sha256'
    Write-SzuUtf8 $temporaryHash "$hash  $name.zip`n"
    $temporaryResult = Join-Path $stageRoot 'candidate.package.json'
    Write-SzuUtf8 $temporaryResult (([ordered]@{ schemaVersion = 1; status = 'success'; version = $version.Version; build = $version.Build; architecture = 'x64'; executableSha256 = $exe.Sha256; zipSha256 = $hash; standaloneFile = "$name.exe"; fileCount = $files.Count + 1; nativeOfflineSmokePassed = $true; signingPerformedByScript = $false }) | ConvertTo-Json -Depth 5)
    $temporaryStandalone = Join-Path $stageRoot 'candidate.exe'
    [IO.File]::Copy($exe.Path, $temporaryStandalone, $false)
    if ((Get-FileHash -LiteralPath $temporaryStandalone -Algorithm SHA256).Hash.ToLowerInvariant() -ne $exe.Sha256) { throw 'Standalone executable differs from the tested executable.' }
    $temporaryStandaloneHash = Join-Path $stageRoot 'candidate.exe.sha256'
    Write-SzuUtf8 $temporaryStandaloneHash "$($exe.Sha256)  $name.exe`n"
    $moves = @(
        @{ Source = $temporaryZip; Destination = $zip },
        @{ Source = $temporaryHash; Destination = $hashPath },
        @{ Source = $temporaryResult; Destination = $resultPath },
        @{ Source = $temporaryStandalone; Destination = $standalone },
        @{ Source = $temporaryStandaloneHash; Destination = $standaloneHash }
    )
    foreach ($move in $moves) {
        [IO.File]::Move($move.Source, $move.Destination)
        $published.Add($move.Destination)
    }
    Write-Host "Package: $zip"
    Write-Host "SHA-256: $hash"
    $global:LASTEXITCODE = 0
} catch {
    foreach ($path in $published) { [IO.File]::Delete($path) }
    $global:LASTEXITCODE = 1
    throw
} finally {
    if ($stageRoot -and (Test-Path -LiteralPath $stageRoot)) { [IO.Directory]::Delete($stageRoot, $true) }
    if ($lock) { $lock.Dispose(); [IO.File]::Delete($lockPath) }
}
