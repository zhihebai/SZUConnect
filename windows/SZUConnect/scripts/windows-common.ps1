# Shared build/package helpers. All generated data stays outside the repository.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-SzuContext {
    $project = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
    $repo = [IO.Path]::GetFullPath((Split-Path -Parent (Split-Path -Parent $project)))
    $digest = [Security.Cryptography.SHA256]::Create()
    try {
        $id = [BitConverter]::ToString($digest.ComputeHash([Text.Encoding]::UTF8.GetBytes($project))).Replace('-', '').Substring(0, 12).ToLowerInvariant()
    } finally { $digest.Dispose() }
    return [pscustomobject]@{ Project = $project; Repo = $repo; DefaultBuildDir = (Join-Path ([IO.Path]::GetTempPath()) "SZUConnect-build\$id\windows-x64") }
}

function Assert-SzuWindows {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'This script requires Windows. Portable core tests do not replace the Windows build or native smoke check.'
    }
}

function Get-SzuExternalDirectory([string]$Path, [string]$Repo) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'An output directory is required.' }
    $separators = [char[]]@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $normalized = [IO.Path]::GetFullPath($Path)
    if ($normalized -eq [IO.Path]::GetPathRoot($normalized)) { throw 'A filesystem root cannot be a build or output directory.' }
    $full = $normalized.TrimEnd($separators)
    $root = [IO.Path]::GetFullPath($Repo).TrimEnd($separators)
    if ($full.Equals($root, [StringComparison]::OrdinalIgnoreCase) -or
        $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Build, report and package directories must be outside the repository.'
    }
    $cursor = $full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Output directories cannot use reparse-point ancestors.' }
            if ($cursor -eq $full -and -not $item.PSIsContainer) { throw 'Output path is not a directory.' }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    return $full
}

function Read-SzuUtf8([string]$Path) { return [IO.File]::ReadAllText($Path, [Text.UTF8Encoding]::new($false, $true)) }
function Write-SzuUtf8([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false)) }

function Get-SzuVersion($Context) {
    $cmake = Read-SzuUtf8 (Join-Path $Context.Project 'CMakeLists.txt')
    $versionMatches = [regex]::Matches($cmake, '(?m)^\s*project\(SZUConnect\s+VERSION\s+(\d+)\.(\d+)\.(\d+)\.(\d+)\s')
    if ($versionMatches.Count -ne 1) { throw 'CMakeLists.txt must define one four-part SZUConnect project version.' }
    $parts = @(1..4 | ForEach-Object { [int]$versionMatches[0].Groups[$_].Value })
    $version = ($parts[0..2] -join '.')
    $build = [string]$parts[3]
    $numeric = ($parts -join '.')
    $rc = Read-SzuUtf8 (Join-Path $Context.Project 'resources/SZUConnect.rc')
    foreach ($field in 'FILEVERSION', 'PRODUCTVERSION') {
        $resourceMatch = [regex]::Match($rc, "(?m)^\s*$field\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*$")
        if (-not $resourceMatch.Success -or ((1..4 | ForEach-Object { [int]$resourceMatch.Groups[$_].Value }) -join '.') -ne $numeric) {
            throw "$field does not match the CMake project version."
        }
    }
    $controller = Read-SzuUtf8 (Join-Path $Context.Project 'app/controller.hpp')
    if (-not [regex]::IsMatch($controller, 'kVersion\s*=\s*"' + [regex]::Escape($version) + '"') -or
        -not [regex]::IsMatch($controller, 'kBuild\s*=\s*"' + [regex]::Escape($build) + '"')) { throw 'The app version constants do not match CMake.' }
    [xml]$manifest = Read-SzuUtf8 (Join-Path $Context.Project 'resources/app.manifest')
    if ([string]$manifest.assembly.assemblyIdentity.version -ne $numeric) { throw 'The app manifest version does not match CMake.' }
    return [pscustomobject]@{ Version = $version; Build = $build; Numeric = $numeric; Parts = $parts }
}

function Get-SzuExecutable([string]$BuildDir, [string]$Configuration, $Version) {
    $path = Join-Path $BuildDir "$Configuration/SZUConnect.exe"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'SZUConnect.exe was not built in the requested build directory.' }
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
    $file = @($info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart) -join '.'
    $product = @($info.ProductMajorPart, $info.ProductMinorPart, $info.ProductBuildPart, $info.ProductPrivatePart) -join '.'
    if ($file -ne $Version.Numeric -or $product -ne $Version.Numeric -or
        $info.FileVersion -ne $Version.Numeric -or $info.ProductVersion -ne "$($Version.Version) ($($Version.Build))") {
        throw 'Executable version resources do not match the source version.'
    }
    $stream = [IO.File]::OpenRead($path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $stream.Position = 0x3c
        $offset = $reader.ReadInt32()
        if ($offset -lt 0 -or $offset + 6 -gt $stream.Length) { throw 'Invalid PE header.' }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'The package requires a Windows x64 executable.' }
    } finally { $reader.Dispose(); $stream.Dispose() }
    return [pscustomobject]@{ Path = $path; Sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant(); FileVersion = $file }
}

function Invoke-SzuNative([string]$Program, [string[]]$Arguments, [string]$Label) {
    & $Program @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Label failed (exit $LASTEXITCODE)." }
}

function Get-SzuPython {
    $command = Get-Command python -ErrorAction SilentlyContinue
    if ($command) { return [pscustomobject]@{ Program = $command.Source; Prefix = @('-B', '-X', 'utf8') } }
    $command = Get-Command py -ErrorAction SilentlyContinue
    if ($command) { return [pscustomobject]@{ Program = $command.Source; Prefix = @('-3', '-B', '-X', 'utf8') } }
    throw 'Python 3 is required for fixture, shared-resource and ZIP checks. Install Python 3 or use the Windows CI artifact.'
}

function Assert-SzuReceipt($Receipt, $Version) {
    foreach ($field in 'schemaVersion', 'version', 'build', 'mode', 'passed', 'failure', 'nativePaintedPages', 'fakeAuthPassed', 'formalServicesCreated', 'checks', 'isolation') {
        if (-not $Receipt.PSObject.Properties[$field]) { throw "Smoke receipt is missing $field." }
    }
    if (($Receipt.schemaVersion -isnot [int] -and $Receipt.schemaVersion -isnot [long]) -or $Receipt.schemaVersion -ne 2 -or
        $Receipt.version -isnot [string] -or $Receipt.version -cne $Version.Version -or
        $Receipt.build -isnot [string] -or $Receipt.build -cne $Version.Build -or
        $Receipt.mode -isnot [string] -or $Receipt.mode -cne 'offlineSmoke' -or $Receipt.failure -isnot [string] -or $Receipt.failure -ne '') {
        throw 'Smoke receipt schema, version, mode or failure field is invalid.'
    }
    foreach ($value in @($Receipt.passed, $Receipt.fakeAuthPassed)) {
        if ($value -isnot [bool] -or -not $value) { throw 'Native smoke or fake authentication did not pass.' }
    }
    if (($Receipt.formalServicesCreated -isnot [int] -and $Receipt.formalServicesCreated -isnot [long]) -or $Receipt.formalServicesCreated -ne 0) { throw 'Smoke mode created formal services.' }
    $pages = @($Receipt.nativePaintedPages)
    if ($pages.Count -ne 3 -or (@($pages | Where-Object { $_ -isnot [string] }).Count -ne 0) -or
        (@($pages | Sort-Object -Unique) -join ',') -cne 'connection,records,settings') { throw 'Smoke receipt does not confirm all three native-painted pages.' }
    foreach ($field in 'windowCreated', 'canvasInitialized', 'cleanShutdown') {
        if (-not $Receipt.checks.PSObject.Properties[$field] -or $Receipt.checks.$field -isnot [bool] -or -not $Receipt.checks.$field) { throw "Native smoke check failed: $field." }
    }
    foreach ($field in 'renderFrames', 'sceneWidgets', 'fakeProbeCalls', 'fakeAuthSubmissions') {
        if (-not $Receipt.checks.PSObject.Properties[$field] -or
            ($Receipt.checks.$field -isnot [int] -and $Receipt.checks.$field -isnot [long]) -or $Receipt.checks.$field -le 0) { throw "Native smoke count is invalid: $field." }
    }
    if ($Receipt.checks.renderFrames -lt 3 -or $Receipt.checks.fakeAuthSubmissions -ne 1) { throw 'Native frames or single fake-auth submission check failed.' }
    $expectedFrames = @('connection', 'records', 'settings', 'settings-detail', 'connection-wide', 'records-wide', 'settings-wide', 'connection-detail', 'connection-dark', 'settings-dark', 'connection-text150', 'text-readability', 'about', 'about-detail', 'about-text150', 'help', 'help-expanded', 'help-detail', 'help-text150', 'network', 'network-text150', 'records-detail', 'result-chips', 'result-chips-text150')
    if (-not $Receipt.PSObject.Properties['visualQA'] -or $Receipt.visualQA.available -isnot [bool] -or -not $Receipt.visualQA.available) {
        throw 'Native visual layout captures are unavailable.'
    }
    $frameLabels = @($Receipt.visualQA.frames | ForEach-Object { $_.page })
    if ($frameLabels.Count -ne $expectedFrames.Count -or (@($frameLabels | Sort-Object -Unique) -join ',') -cne (@($expectedFrames | Sort-Object) -join ',')) {
        throw 'Native visual layout scenarios are incomplete.'
    }
    if ($Receipt.checks.aboutLinksChecked -ne 6) { throw 'About canonical link checks are incomplete.' }
    if ($Receipt.visualQA.expectedFrames -ne $expectedFrames.Count -or $Receipt.checks.checkedLayouts -ne $expectedFrames.Count -or
        $Receipt.checks.fixedSheetHeadersChecked -ne 7 -or $Receipt.checks.helpLayoutsChecked -ne 4 -or
        $Receipt.checks.recordsLayoutsChecked -ne 3 -or $Receipt.checks.networkLayoutsChecked -ne 7) {
        throw 'Native sheet, disclosure, or record layout checks are incomplete.'
    }
    if ($Receipt.checks.navigationTextLayoutsChecked -le 0 -or $Receipt.checks.navigationIconLayoutsChecked -le 0 -or
        $Receipt.checks.navigationBrandLayoutsChecked -ne 3 -or
        ($Receipt.checks.navigationTextLayoutsChecked + $Receipt.checks.navigationIconLayoutsChecked + $Receipt.checks.navigationBrandLayoutsChecked) -ne $expectedFrames.Count -or
        $Receipt.checks.sectionTitleSpacingChecked -ne 21) { throw 'Navigation tiers or connection title spacing were not verified.' }
    if ($Receipt.checks.resultChipLayoutsChecked -ne 4 -or $Receipt.checks.stackedFooterLayoutsChecked -ne 2) {
        throw 'Result chip bounds or wrapped-footer spacing were not verified.'
    }
    if (-not $Receipt.visualQA.PSObject.Properties['textRenderingProfiles'] -or
        $Receipt.visualQA.textRenderingProfiles -isnot [array] -or
        $Receipt.visualQA.textRenderingProfiles.Count -lt 1 -or $Receipt.visualQA.textRenderingProfiles.Count -gt $expectedFrames.Count) {
        throw 'Native text rendering profiles are missing or invalid.'
    }
    $renderingProfiles = $Receipt.visualQA.textRenderingProfiles
    $profileFields = @('antialiasMode', 'renderingMode', 'pixelGeometry', 'fontFamily', 'primaryFontHasCjk', 'dpi', 'gamma', 'enhancedContrast', 'clearTypeLevel', 'opaqueTarget', 'monitorParameters', 'customParameters')
    $profileKeys = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $usedProfiles = [Collections.Generic.HashSet[int]]::new()
    foreach ($renderingProfile in $renderingProfiles) {
        if ($renderingProfile -isnot [pscustomobject] -or
            (@($renderingProfile.PSObject.Properties.Name | Sort-Object) -join ',') -cne (@($profileFields | Sort-Object) -join ',')) {
            throw 'A native text rendering profile does not contain the complete schema.'
        }
        foreach ($field in 'antialiasMode', 'renderingMode', 'pixelGeometry', 'fontFamily') {
            if ($renderingProfile.$field -isnot [string] -or [string]::IsNullOrWhiteSpace($renderingProfile.$field)) { throw "Native text profile string is invalid: $field." }
        }
        foreach ($field in 'primaryFontHasCjk', 'opaqueTarget', 'monitorParameters', 'customParameters') {
            if ($renderingProfile.$field -isnot [bool]) { throw "Native text profile boolean is invalid: $field." }
        }
        foreach ($field in 'dpi', 'gamma', 'enhancedContrast', 'clearTypeLevel') {
            $value = $renderingProfile.$field
            if (($value -isnot [int] -and $value -isnot [long] -and $value -isnot [double] -and $value -isnot [decimal]) -or
                [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value)) { throw "Native text profile number is invalid: $field." }
        }
        if ($renderingProfile.renderingMode -cne 'automatic' -or $renderingProfile.antialiasMode -cne 'grayscale' -or
            $renderingProfile.pixelGeometry -cnotin @('flat', 'rgb', 'bgr') -or -not $renderingProfile.opaqueTarget -or -not $renderingProfile.customParameters -or
            $renderingProfile.dpi -le 0 -or $renderingProfile.gamma -le 0 -or $renderingProfile.enhancedContrast -lt 0 -or
            $renderingProfile.clearTypeLevel -lt 0 -or $renderingProfile.clearTypeLevel -gt 1) {
            throw 'Native text rendering profile values are invalid.'
        }
        $canonicalProfile = [ordered]@{}
        foreach ($field in $profileFields) { $canonicalProfile[$field] = $renderingProfile.$field }
        if (-not $profileKeys.Add(($canonicalProfile | ConvertTo-Json -Depth 3 -Compress))) { throw 'Native text rendering profiles are duplicated.' }
    }
    foreach ($frame in $Receipt.visualQA.frames) {
        if (-not $frame.PSObject.Properties['textRenderingProfile'] -or $frame.PSObject.Properties['textRendering'] -or
            ($frame.textRenderingProfile -isnot [int] -and $frame.textRenderingProfile -isnot [long]) -or
            $frame.textRenderingProfile -lt 0 -or $frame.textRenderingProfile -ge $renderingProfiles.Count) {
            throw 'A native frame has an invalid text rendering profile index.'
        }
        $rendering = $renderingProfiles[[int]$frame.textRenderingProfile]
        [void]$usedProfiles.Add([int]$frame.textRenderingProfile)
        if ($frame.headerEdgePixelSamples -ne 9) { throw 'The common toolbar/caption bottom edge was not painted continuously.' }
        if ($frame.navigationTier -cnotin @('text', 'icons', 'brand') -or $frame.brandingPixelSamples -lt 12 -or
            ($frame.navigationTier -ceq 'brand' -and $frame.appIconPainted) -or
            ($frame.navigationTier -cne 'brand' -and -not $frame.appIconPainted)) { throw 'The visible navigation branding was not painted.' }
        $aboutFrame = @('about', 'about-detail', 'about-text150') -ccontains $frame.page
        $helpFrame = @('help', 'help-expanded', 'help-detail', 'help-text150') -ccontains $frame.page
        if ($aboutFrame -or $helpFrame) {
            $expectedLinks = if ($aboutFrame) { 2 } else { 0 }
            if ($frame.selectionPixelCheck -cne 'notApplicableModal' -or $frame.selectionPixelSamples -ne 0 -or $frame.aboutLinksChecked -ne $expectedLinks) { throw 'Modal link or selection applicability check failed.' }
        } elseif ($frame.selectionPixelCheck -cne 'sampled' -or $frame.selectionPixelSamples -le 0) {
            throw 'Native choice selection pixels were not sampled.'
        }
        if ($rendering.renderingMode -cne 'automatic' -or
            $rendering.antialiasMode -cne 'grayscale' -or
            -not $rendering.opaqueTarget -or -not $rendering.customParameters) {
            throw 'Native automatic grayscale text or application icon was not painted.'
        }
    }
    if ($usedProfiles.Count -ne $renderingProfiles.Count) { throw 'Native text rendering profiles contain unused entries.' }
    if (-not $Receipt.isolation.PSObject.Properties['passed'] -or $Receipt.isolation.passed -isnot [bool] -or -not $Receipt.isolation.passed) { throw 'Smoke isolation did not pass.' }
    foreach ($field in 'productionStorageAccesses', 'vaultAccesses', 'networkCalls', 'wifiCalls', 'watchersStarted', 'autostartWrites') {
        if (-not $Receipt.isolation.PSObject.Properties[$field] -or
            ($Receipt.isolation.$field -isnot [int] -and $Receipt.isolation.$field -isnot [long]) -or $Receipt.isolation.$field -ne 0) { throw "Smoke isolation counter is invalid: $field." }
    }
}
