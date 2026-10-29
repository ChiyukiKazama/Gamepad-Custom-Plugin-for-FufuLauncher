#requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-ReleaseVersion([string]$Version) {
    if ($Version -cnotmatch '\A(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\z') {
        throw 'Version must be major.minor.patch, for example 1.2.3 (no prefixes, suffixes or whitespace).'
    }
    foreach ($part in $Version.Split('.')) {
        [uint16]$number = 0
        if (-not [uint16]::TryParse($part, [ref]$number)) { throw 'PE version components must be in 0..65535.' }
    }
}

function Read-Ini([string]$Text) {
    $sections = @{}
    $current = $null
    foreach ($line in ($Text -split '\r?\n')) {
        $trimmed = $line.Trim().TrimStart([char]0xfeff)
        if (-not $trimmed -or $trimmed.StartsWith(';') -or $trimmed.StartsWith('#')) { continue }
        if ($trimmed -match '^\[([^\]]+)\]$') {
            $current = $Matches[1]
            if ($sections.ContainsKey($current)) { throw "Duplicate INI section: $current" }
            $sections[$current] = @{}
        } elseif ($null -ne $current -and $trimmed -match '^([^=]+?)\s*=\s*(.*)$') {
            $key = $Matches[1].Trim()
            if ($sections[$current].ContainsKey($key)) { throw "Duplicate INI key: $current.$key" }
            $sections[$current][$key] = $Matches[2].Trim()
        } else { throw "Invalid INI line: $line" }
    }
    return $sections
}

function Assert-PluginConfig([string]$Text, [string]$Version) {
    $ini = Read-Ini $Text
    $expected = @('General', 'MapBinding', 'MapButton', 'GamepadSensitivityMultiplier', 'DualSenseEdgeView')
    if (@(Compare-Object $expected @($ini.Keys)).Count) { throw 'Unexpected plugin configuration sections.' }
    if ($ini.General.File -cne 'GamepadCustomPlugin.dll' -or $ini.General.Version -cne $Version) {
        throw 'Config DLL name or version differs.'
    }
    if ($ini.MapBinding.Type -cne 'bool' -or $ini.MapBinding.Value -cne '1' -or
        $ini.MapButton.Type -cne 'enum' -or $ini.MapButton.OptionsSource -cne 'gamepad' -or
        $ini.MapButton.Value -cne 'View' -or $ini.GamepadSensitivityMultiplier.Type -cne 'string' -or
        $ini.GamepadSensitivityMultiplier.Value -cne '1.0' -or $ini.DualSenseEdgeView.Type -cne 'bool' -or
        $ini.DualSenseEdgeView.Value -cne '1') {
        throw 'The package must use the repository defaults for map binding, gamepad button, sensitivity and the DualSense Edge view.'
    }
    return $ini
}

function Get-ReleaseContext([string]$ProjectRoot, [string]$Version = '') {
    $metadata = Get-Content -LiteralPath (Join-Path $ProjectRoot 'store/metadata.json') -Raw -Encoding utf8 | ConvertFrom-Json
    Assert-ReleaseVersion $metadata.version
    $text = [IO.File]::ReadAllText((Join-Path $ProjectRoot 'config.ini'))
    Assert-PluginConfig $text $metadata.version | Out-Null
    # Store display text is maintained independently of the plugin settings UI.
    # Only release identity, base version and filenames must agree.
    if ($metadata.id -cne 'XboxGamepadPlugin' -or $metadata.dll_file_name -cne 'GamepadCustomPlugin.dll' -or
        $metadata.archive_file_name -cne "GamepadCustomPlugin-$($metadata.version).zip") {
        throw 'Store plugin ID, DLL filename or base archive filename differs.'
    }
    if ($Version -ceq '') { $Version = [string]$metadata.version }
    Assert-ReleaseVersion $Version
    # Replace only the General.Version value in the generated config.
    $general = [regex]::Match($text, '(?ms)^\[General\][^\r\n]*\r?\n.*?(?=^\[|\z)')
    $match = [regex]::Match($general.Value, '(?m)^Version\s*=\s*([^\s]+)')
    if (-not $match.Success) { throw 'General.Version is missing.' }
    $value = $match.Groups[1]
    $config = $text.Remove($general.Index + $value.Index, $value.Length).Insert($general.Index + $value.Index, $Version)
    $metadata.version = $Version
    $metadata.archive_file_name = "GamepadCustomPlugin-$Version.zip"
    return [pscustomobject]@{ Version = $Version; Tag = "v$Version"; Config = $config; Metadata = $metadata }
}

function Get-BytesSha256([byte[]]$Bytes) {
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}

function Assert-X64Dll([byte[]]$Bytes) {
    if ($Bytes.Length -lt 64 -or [BitConverter]::ToUInt16($Bytes, 0) -ne 0x5a4d) { throw 'Invalid DLL DOS header.' }
    $pe = [BitConverter]::ToInt32($Bytes, 60)
    if ($pe -lt 64 -or $pe -gt $Bytes.Length - 264 -or [BitConverter]::ToUInt32($Bytes, $pe) -ne 0x4550 -or
        [BitConverter]::ToUInt16($Bytes, $pe + 4) -ne 0x8664 -or
        ([BitConverter]::ToUInt16($Bytes, $pe + 22) -band 0x2002) -ne 0x2002 -or
        [BitConverter]::ToUInt16($Bytes, $pe + 24) -ne 0x20b) { throw 'Expected a valid AMD64 PE32+ DLL.' }
    $sections = [BitConverter]::ToUInt16($Bytes, $pe + 6)
    $optionalSize = [BitConverter]::ToUInt16($Bytes, $pe + 20)
    $table = $pe + 24 + $optionalSize
    if ($sections -eq 0 -or $optionalSize -lt 240 -or $table + 40 * $sections -gt $Bytes.Length) {
        throw 'Invalid PE section table.'
    }
    for ($i = 0; $i -lt $sections; $i++) {
        $size = [BitConverter]::ToUInt32($Bytes, $table + 40 * $i + 16)
        $offset = [BitConverter]::ToUInt32($Bytes, $table + 40 * $i + 20)
        if ([long]$offset + $size -gt $Bytes.Length) { throw 'PE section extends past end of DLL.' }
    }
}

function Assert-DllVersion([string]$Path, [string]$Version) {
    Assert-X64Dll ([IO.File]::ReadAllBytes($Path))
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    $parts = @($Version.Split('.') | ForEach-Object { [int]$_ })
    if ($info.FileVersion -cne $Version -or $info.ProductVersion -cne $Version -or
        $info.OriginalFilename -cne 'GamepadCustomPlugin.dll' -or
        $info.FileMajorPart -ne $parts[0] -or $info.FileMinorPart -ne $parts[1] -or
        $info.FileBuildPart -ne $parts[2] -or $info.FilePrivatePart -ne 0) {
        throw "DLL version resource differs from $Version."
    }
}
