$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$version = 'v0.1.0'
$assetsDir = Join-Path $repo 'release-assets'
New-Item -ItemType Directory -Path $assetsDir -Force | Out-Null

$pluginSrc = Join-Path $repo 'ue5-mcp-plugin'
$serverSrc = Join-Path $repo 'mcp_server'
$pluginZip = Join-Path $assetsDir ("ue5-mcp-plugin-$version.zip")
$serverZip = Join-Path $assetsDir ("mcp_server-$version.zip")

if (Test-Path $pluginZip) { Remove-Item $pluginZip -Force }
if (Test-Path $serverZip) { Remove-Item $serverZip -Force }

Compress-Archive -Path $pluginSrc -DestinationPath $pluginZip -Force -CompressionLevel Optimal
Compress-Archive -Path $serverSrc -DestinationPath $serverZip -Force -CompressionLevel Optimal

Get-Item $pluginZip, $serverZip | Select-Object FullName, Length
