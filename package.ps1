# Packt SPS.dll als Vortex-installierbares Archiv:
#   package.ps1      -> build\release\SPS.dll -> dist\SPS-<version>.zip     (Skyrim SE/AE)
#   package.ps1 -Vr  -> build\vr\SPS.dll      -> dist\SPS-VR-<version>.zip  (Skyrim VR, eigene Nexus-Datei)
#   package.ps1 -Profile -> build\profile\SPS.dll -> dist\SPS-PROFILE-<version>-NOT-FOR-RELEASE.zip
#                        (Mess-Build mit Tracy, nur lokal zum Einbinden in Vortex - NIE hochladen)
param([switch]$Vr, [switch]$Profile)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$version = (Get-Content "$root\vcpkg.json" -Raw | ConvertFrom-Json).'version-semver'
$build = if ($Profile) { "$root\build\profile" } elseif ($Vr) { "$root\build\vr" } else { "$root\build\release" }
$name = if ($Profile) { "SPS-PROFILE-$version-NOT-FOR-RELEASE" } elseif ($Vr) { "SPS-VR-OCCLUSION-TEST-$version" } else { "SPS-OCCLUSION-TEST-$version" }
# Sperre: Release-DLL darf weder Netzwerk-Bibliotheken noch den Tracy-Profiler enthalten (1.0.3 oeffnete einen
# Netzwerk-Port -> Firewall-Abfrage bei Nutzern). Entwickler-Builds mit -DSPS_TRACY=ON werden hier abgewiesen.
$dllText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes("$build\SPS.dll"))
if ($Profile) {
    if ($dllText.IndexOf('Tracy', [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "build\profile\SPS.dll enthaelt kein Tracy - ist das wirklich der Mess-Build?"
    }
} else { foreach ($bad in 'ws2_32', 'wsock32', 'wininet', 'winhttp', 'urlmon', 'mswsock', 'dnsapi', 'iphlpapi', 'Tracy') {
    if ($dllText.IndexOf($bad, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "SPS.dll enthaelt '$bad' - Netzwerk/Tracy darf nicht ins Release (mit SPS_TRACY=OFF neu bauen)"
    }
} }
$stage = Join-Path $root 'dist\stage'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\SKSE\Plugins" | Out-Null
# PDB (~150 MB, nur fuer Absturzanalyse) nicht ins Mod-Paket - separat als dist\<name>-pdb.zip
Copy-Item "$build\SPS.dll", "$root\package\SPS.ini" "$stage\SKSE\Plugins"
$zip = Join-Path $root "dist\$name.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
# Eintraege mit "/" (Compress-Archive in PowerShell 5.1 schreibt "\" - manche Mod-Manager/Tools stolpern darueber)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    Get-ChildItem $stage -Recurse -File | ForEach-Object {
        $entry = $_.FullName.Substring($stage.Length + 1).Replace([char]92, [char]47)
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry, 'Optimal') | Out-Null
    }
} finally {
    $archive.Dispose()
}
Remove-Item $stage -Recurse -Force
Write-Host "Paket: $zip"
if ($Profile) { return }  # Mess-Build: PDB bleibt in build\profile
$pdbZip = Join-Path $root "dist\$name-pdb.zip"
Remove-Item $pdbZip -ErrorAction SilentlyContinue
Compress-Archive -Path "$build\SPS.pdb" -DestinationPath $pdbZip
Write-Host "Debug-Symbole: $pdbZip"
