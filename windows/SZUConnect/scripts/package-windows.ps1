# Release package only. Build evidence stays outside the single-file ZIP.
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
    $name = "SZUConnect-$($version.Version)-win-x64"
    $zip = Join-Path $OutputDir "$name.zip"
    $hashPath = $zip + '.sha256'
    $resultPath = Join-Path $OutputDir "$name.package.json"
    $standalone = Join-Path $OutputDir "$name.exe"
    $standaloneHash = $standalone + '.sha256'
    $manifestPath = Join-Path $OutputDir "$name.manifest.json"
    $smokePath = Join-Path $OutputDir "$name.smoke.json"
    foreach ($path in @($zip, $hashPath, $resultPath, $standalone, $standaloneHash, $manifestPath, $smokePath)) {
        if (Test-Path -LiteralPath $path) { throw 'A package output already exists. Choose a new OutputDir; existing artifacts are retained.' }
    }
    [IO.Directory]::CreateDirectory($OutputDir) | Out-Null
    $lockPath = Join-Path $OutputDir ('.' + $name + '.lock')
    $lock = [IO.File]::Open($lockPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $python = Get-SzuPython
    Invoke-SzuNative $python.Program (@($python.Prefix) + @((Join-Path $PSScriptRoot 'verify-package-tests.py'))) 'Package verifier regression tests'
    $smoke = & (Join-Path $PSScriptRoot 'test-windows.ps1') -Configuration Release -BuildDir $BuildDir -ReportDir $ReportDir -SkipBuild:$SkipBuild
    $exe = Get-SzuExecutable $BuildDir 'Release' $version
    if ($exe.Sha256 -ne $smoke.ExecutableSha256) { throw 'Executable changed after testing.' }
    $stageRoot = Join-Path $OutputDir ('.szu-package-' + [Guid]::NewGuid().ToString('N'))
    $stage = Join-Path $stageRoot $name
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    [IO.File]::Copy($exe.Path, (Join-Path $stage 'SZUConnect.exe'), $false)
    $temporarySmoke = Join-Path $stageRoot "$name.smoke.json"
    [IO.File]::Copy($smoke.ReceiptPath, $temporarySmoke, $false)
    $embeddedDocuments = @(
        @{ Id = 401; Source = 'docs/PACKAGE-WINDOWS.md' },
        @{ Id = 402; Source = 'LICENSE' },
        @{ Id = 403; Source = 'docs/DISTRIBUTION-NOTICES.txt' },
        @{ Id = 404; Source = 'docs/licenses/curl-COPYING.txt' }
    ) | ForEach-Object {
        $source = Join-Path $context.Repo $_.Source
        [ordered]@{ resourceId = $_.Id; source = $_.Source; size = (Get-Item -LiteralPath $source).Length; sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    $files = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Force | ForEach-Object {
        $relative = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [ordered]@{ name = $relative; size = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    } | Sort-Object { $_.name })
    if (($files | Where-Object { $_.name -eq 'SZUConnect.exe' }).sha256 -ne $exe.Sha256) { throw 'Staged executable bytes differ from the tested executable.' }
    $manifest = [ordered]@{
        schemaVersion = 2; version = $version.Version; build = $version.Build; architecture = 'x64'; configuration = 'Release'
        packageRoot = $name; signingPerformedByScript = $false; files = $files; embeddedDocuments = @($embeddedDocuments)
        nativeOfflineSmoke = [ordered]@{
            name = "$name.smoke.json"; size = (Get-Item -LiteralPath $temporarySmoke).Length
            sha256 = (Get-FileHash -LiteralPath $temporarySmoke -Algorithm SHA256).Hash.ToLowerInvariant(); executableSha256 = $exe.Sha256
        }
    }
    $temporaryManifest = Join-Path $stageRoot "$name.manifest.json"
    Write-SzuUtf8 $temporaryManifest ($manifest | ConvertTo-Json -Depth 10)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $temporaryZip = Join-Path $stageRoot 'candidate.zip'
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $temporaryZip, [IO.Compression.CompressionLevel]::Optimal, $true)
    Invoke-SzuNative $python.Program (@($python.Prefix) + @((Join-Path $PSScriptRoot 'verify-package.py'), '--archive', $temporaryZip, '--expected-root', $name, '--manifest', $temporaryManifest, '--expected-exe', $exe.Path, '--source-root', $context.Repo)) 'ZIP inventory, CRC, hash and embedded license verification'
    $hash = (Get-FileHash -LiteralPath $temporaryZip -Algorithm SHA256).Hash.ToLowerInvariant()
    $temporaryHash = Join-Path $stageRoot 'candidate.sha256'
    Write-SzuUtf8 $temporaryHash "$hash  $name.zip`n"
    $temporaryResult = Join-Path $stageRoot 'candidate.package.json'
    Write-SzuUtf8 $temporaryResult (([ordered]@{ schemaVersion = 2; status = 'success'; version = $version.Version; build = $version.Build; architecture = 'x64'; executableSha256 = $exe.Sha256; zipSha256 = $hash; standaloneFile = "$name.exe"; fileCount = $files.Count; manifestFile = "$name.manifest.json"; smokeReceiptFile = "$name.smoke.json"; embeddedDocumentsVerified = $true; nativeOfflineSmokePassed = $true; signingPerformedByScript = $false }) | ConvertTo-Json -Depth 5)
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
        @{ Source = $temporaryStandaloneHash; Destination = $standaloneHash },
        @{ Source = $temporaryManifest; Destination = $manifestPath },
        @{ Source = $temporarySmoke; Destination = $smokePath }
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
