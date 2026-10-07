# Packt build\release\SPS.dll als Vortex-installierbares Archiv nach dist\SPS-<version>.zip
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$version = (Get-Content "$root\vcpkg.json" -Raw | ConvertFrom-Json).'version-semver'
# Sperre: Release-DLL darf weder Netzwerk-Bibliotheken noch den Tracy-Profiler enthalten (1.0.3 oeffnete einen
# Netzwerk-Port -> Firewall-Abfrage bei Nutzern). Entwickler-Builds mit -DSPS_TRACY=ON werden hier abgewiesen.
$dllText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes("$root\build\release\SPS.dll"))
foreach ($bad in 'ws2_32', 'wsock32', 'wininet', 'winhttp', 'urlmon', 'mswsock', 'dnsapi', 'iphlpapi', 'Tracy') {
    if ($dllText.IndexOf($bad, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "SPS.dll enthaelt '$bad' - Netzwerk/Tracy darf nicht ins Release (mit SPS_TRACY=OFF neu bauen)"
    }
}
$stage = Join-Path $root 'dist\stage'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\SKSE\Plugins" | Out-Null
# PDB (~150 MB, nur fuer Absturzanalyse) nicht ins Mod-Paket - separat als dist\SPS-<version>-pdb.zip
Copy-Item "$root\build\release\SPS.dll", "$root\package\SPS.ini" "$stage\SKSE\Plugins"
$zip = Join-Path $root "dist\SPS-$version.zip"
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
$pdbZip = Join-Path $root "dist\SPS-$version-pdb.zip"
Remove-Item $pdbZip -ErrorAction SilentlyContinue
Compress-Archive -Path "$root\build\release\SPS.pdb" -DestinationPath $pdbZip
Write-Host "Debug-Symbole: $pdbZip"
