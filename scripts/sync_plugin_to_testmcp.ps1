param(
    [Parameter(Mandatory = $false)]
    [string]$ProjectDir = $env:UE_TESTMCP_PROJECT_DIR
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ProjectDir)) {
    Write-Host "사용법: .\scripts\sync_plugin_to_testmcp.ps1 -ProjectDir <언리얼_프로젝트_경로>"
    Write-Host "또는 환경변수 UE_TESTMCP_PROJECT_DIR 설정"
    exit 1
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$SourcePluginDir = Join-Path $RepoRoot "ue5-mcp-plugin"
$ResolvedProjectDir = Resolve-Path $ProjectDir
$TargetPluginsDir = Join-Path $ResolvedProjectDir "Plugins"
$TargetPluginDir = Join-Path $TargetPluginsDir "ue5-mcp-plugin"

if (-not (Test-Path $SourcePluginDir)) {
    throw "소스 플러그인 경로를 찾을 수 없습니다: $SourcePluginDir"
}

if (-not (Test-Path $ResolvedProjectDir)) {
    throw "대상 프로젝트 경로를 찾을 수 없습니다: $ResolvedProjectDir"
}

New-Item -ItemType Directory -Path $TargetPluginsDir -Force | Out-Null
if (Test-Path $TargetPluginDir) {
    Remove-Item -Recurse -Force $TargetPluginDir
}

Copy-Item -Recurse -Force $SourcePluginDir $TargetPluginDir

$cleanupDirs = @(
    (Join-Path $TargetPluginDir "Binaries"),
    (Join-Path $TargetPluginDir "Intermediate"),
    (Join-Path $TargetPluginDir "DerivedDataCache"),
    (Join-Path $TargetPluginDir "Saved"),
    (Join-Path $TargetPluginDir ".git")
)

foreach ($dir in $cleanupDirs) {
    if (Test-Path $dir) {
        Remove-Item -Recurse -Force $dir
    }
}

Write-Host "[완료] 플러그인 동기화:"
Write-Host "  from: $SourcePluginDir"
Write-Host "  to  : $TargetPluginDir"
Write-Host "다음 단계: UE 에디터에서 프로젝트를 열고 C++ 빌드를 수행하세요."
