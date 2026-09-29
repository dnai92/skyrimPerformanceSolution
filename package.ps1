# Packt build\release\SkyrimPerf.dll als Vortex-installierbares Archiv nach dist\SkyrimPerf-<version>.zip
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$version = (Get-Content "$root\vcpkg.json" -Raw | ConvertFrom-Json).'version-semver'
$stage = Join-Path $root 'dist\stage'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\SKSE\Plugins" | Out-Null
Copy-Item "$root\build\release\SkyrimPerf.dll", "$root\build\release\SkyrimPerf.pdb" "$stage\SKSE\Plugins"
$zip = Join-Path $root "dist\SkyrimPerf-$version.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Remove-Item $stage -Recurse -Force
Write-Host "Paket: $zip"
