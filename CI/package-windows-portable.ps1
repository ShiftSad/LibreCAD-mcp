# librecad-mcp: pack a windeployqt'd LibreCAD-mcp build as a portable zip and write a
# SHA256SUMS file for the zip and the NSIS installer. Called by .github/workflows/build-all.yml
# after "Generate custom.nsh and Deploy" and "Rename installer".
#
# The zip has the installer's layout (scripts/postprocess-windows/nsis-msvc.nsi):
#   - the windeployqt output from windows\, without *.pdb and translations\;
#   - the Qt platform, image format and style plugins windeployqt may have left out, among
#     them qoffscreen, which headless runs need (tools/remote-api/e2e_smoke.py);
#   - resources\qm, resources\fonts, resources\patterns and resources\library, where
#     RS_System looks next to LibreCAD.exe.
# Its top folder is LibreCAD-mcp\, so it never mixes with an extracted upstream LibreCAD.
#
# Usage: package-windows-portable.ps1 -Arch x64|arm64 -Version <git describe>
#          [-Source windows] [-Installer <renamed installer .exe>] [-OutDir .] [-QtDir <Qt arch dir>]

param(
    [Parameter(Mandatory = $true)][ValidateSet('x64', 'arm64')][string] $Arch,
    [Parameter(Mandatory = $true)][string] $Version,
    [string] $Source = 'windows',
    [string] $Installer = '',
    [string] $OutDir = '.',
    [string] $QtDir = $env:QT_ROOT_DIR
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
$Source = (Resolve-Path -LiteralPath $Source).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
if (-not (Test-Path -LiteralPath (Join-Path $Source 'LibreCAD.exe'))) {
    throw "LibreCAD.exe not found in ${Source}: run windeployqt first"
}

$baseName = "LibreCAD-mcp-$Version-win-$Arch-portable"
$stage = Join-Path ([IO.Path]::GetTempPath()) ("lc-portable-" + [guid]::NewGuid().ToString('N'))
$top = Join-Path $stage 'LibreCAD-mcp'
New-Item -ItemType Directory -Force -Path $top | Out-Null

function Copy-Tree([string] $From, [string] $Filter, [string] $To) {
    # Files matching $Filter under $From, keeping their sub-folders (like NSIS File /r).
    if (-not (Test-Path -LiteralPath $From)) {
        Write-Warning "$From does not exist: nothing copied to $To"
        return
    }
    $root = (Resolve-Path -LiteralPath $From).Path.TrimEnd('\', '/')
    Get-ChildItem -LiteralPath $root -Recurse -File -Filter $Filter | ForEach-Object {
        $target = Join-Path $To $_.FullName.Substring($root.Length + 1)
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
    }
}

try {
    # 1. The windeployqt output, as the installer takes it.
    Get-ChildItem -LiteralPath $Source -Force |
        Where-Object { $_.Name -ne 'translations' -and $_.Extension -ne '.pdb' } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $top -Recurse -Force }
    Get-ChildItem -LiteralPath $top -Recurse -File -Filter '*.pdb' | Remove-Item -Force

    # 2. Qt plugins windeployqt may have left out (the installer's non-fatal fallbacks).
    $plugins = @(
        'platforms\qwindows.dll', 'platforms\qminimal.dll', 'platforms\qoffscreen.dll',
        'imageformats\qgif.dll', 'imageformats\qico.dll', 'imageformats\qjpeg.dll',
        'imageformats\qsvg.dll', 'imageformats\qtiff.dll', 'styles\qwindowsvistastyle.dll',
        'styles\qmodernwindowsstyle.dll'
    )
    if ($QtDir -and (Test-Path -LiteralPath $QtDir)) {
        foreach ($plugin in $plugins) {
            $from = Join-Path $QtDir "plugins\$plugin"
            $to = Join-Path $top $plugin
            if ((Test-Path -LiteralPath $from) -and -not (Test-Path -LiteralPath $to)) {
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
                Copy-Item -LiteralPath $from -Destination $to
            }
        }
    }
    else {
        Write-Warning "Qt folder not found (QT_ROOT_DIR='$QtDir'): only windeployqt's plugins are packed"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $top 'platforms\qoffscreen.dll'))) {
        Write-Warning 'qoffscreen.dll is not in the package: headless runs will need a visible window'
    }

    # 3. Resources, where RS_System::getDirectoryList looks next to LibreCAD.exe.
    $qm = Join-Path $top 'resources\qm'
    New-Item -ItemType Directory -Force -Path $qm | Out-Null
    $translations = @("$repo\librecad\ts\*.qm", "$repo\plugins\ts\*.qm")
    if ($QtDir) {
        $translations += @("$QtDir\translations\qt_*.qm", "$QtDir\translations\qtbase_*.qm")
    }
    foreach ($pattern in $translations) {
        Get-ChildItem -Path $pattern -File -ErrorAction SilentlyContinue | Copy-Item -Destination $qm -Force
    }
    Copy-Tree "$repo\librecad\support\fonts" '*.lff' (Join-Path $top 'resources\fonts')
    Copy-Tree "$repo\librecad\support\patterns" '*.dxf' (Join-Path $top 'resources\patterns')
    Copy-Tree "$repo\librecad\support\library" '*.dxf' (Join-Path $top 'resources\library')
    if (-not (Get-ChildItem -LiteralPath (Join-Path $top 'resources\fonts') -Recurse -File -ErrorAction SilentlyContinue)) {
        throw 'No .lff fonts were packed: text would not render'
    }

    # 4. A short note for whoever unpacks it.
    $readme = @(
        "LibreCAD-mcp $Version (Windows $Arch, portable)",
        '',
        'An unofficial fork of LibreCAD with AutoCAD 2018-2026 DWG reading fixes and an',
        'embedded Remote API for MCP clients such as Claude.',
        '',
        'Start LibreCAD.exe. Nothing is installed. Preferences are stored like those of an',
        'installed LibreCAD 2.2 (registry key LibreCAD\LibreCAD-2), so the two share them.',
        '',
        'The Remote API is off until you enable it in Options > Application Preferences >',
        'Remote API (MCP). It listens on 127.0.0.1 only and needs a token.',
        '',
        'Documentation: https://github.com/ShiftSad/LibreCAD-mcp/blob/master/docs/remote-api/README.md',
        'Problems with this build: https://github.com/ShiftSad/LibreCAD-mcp/issues',
        'Upstream LibreCAD: https://librecad.org',
        'License: GPL-2.0-or-later; the full text is in LICENSE.txt.'
    )
    [IO.File]::WriteAllText((Join-Path $top 'README-LibreCAD-mcp.txt'), (($readme -join "`r`n") + "`r`n"))
    # The GPL travels with the binaries (the installer shows the same file).
    Copy-Item -LiteralPath (Join-Path $repo 'licenses\gpl-2.0.txt') -Destination (Join-Path $top 'LICENSE.txt')

    # 5. The zip.
    $zip = Join-Path $OutDir "$baseName.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    $sevenZip = Get-Command 7z -ErrorAction SilentlyContinue
    if (-not $sevenZip -and (Test-Path 'C:\Program Files\7-Zip\7z.exe')) {
        $sevenZip = Get-Command 'C:\Program Files\7-Zip\7z.exe'
    }
    if ($sevenZip) {
        Push-Location $stage
        try {
            & $sevenZip.Source a -tzip -mx=7 $zip 'LibreCAD-mcp' | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "7-Zip failed ($LASTEXITCODE)" }
        }
        finally { Pop-Location }
    }
    else {
        Compress-Archive -LiteralPath $top -DestinationPath $zip
    }

    # 6. SHA256SUMS in GNU coreutils format ("<hash>  <file>", LF): sha256sum -c works.
    $files = @($zip)
    if ($Installer) {
        if (-not (Test-Path -LiteralPath $Installer)) { throw "installer $Installer not found" }
        $files += (Resolve-Path -LiteralPath $Installer).Path
    }
    $lines = foreach ($file in $files) {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path -Leaf $file)
    }
    $sums = Join-Path $OutDir "SHA256SUMS-win-$Arch.txt"
    [IO.File]::WriteAllText($sums, (($lines -join "`n") + "`n"))
    Get-Content -LiteralPath $sums
    Get-Item -LiteralPath $files | Format-Table Name, Length
}
finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
