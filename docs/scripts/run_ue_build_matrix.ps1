param(
    [Parameter(Mandatory = $false)]
    [string]$ProjectPath = "D:\Codex-cli\ue5-mcp-plugin\TestMcp\TestMcp.uproject",

    [Parameter(Mandatory = $false)]
    [string]$Target = "TestMcpEditor Win64 Development",

    [Parameter(Mandatory = $false)]
    [string[]]$EngineRoots = @(
        "C:\Program Files\Epic Games\UE_5.3",
        "C:\Program Files\Epic Games\UE_5.4",
        "C:\Program Files\Epic Games\UE_5.5",
        "C:\Program Files\Epic Games\UE_5.6",
        "C:\Program Files\Epic Games\UE_5.7"
    ),

    [Parameter(Mandatory = $false)]
    [string]$OutputJson = ".\\build_matrix_result.json"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $ProjectPath)) {
    throw "Project not found: $ProjectPath"
}

$results = @()

foreach ($engineRoot in $EngineRoots) {
    $buildBat = Join-Path $engineRoot "Engine\Build\BatchFiles\Build.bat"
    if (-not (Test-Path $buildBat)) {
        $results += [PSCustomObject]@{
            engine_root = $engineRoot
            status = "skipped"
            reason = "Build.bat not found"
            exit_code = $null
        }
        continue
    }

    Write-Host "[Matrix] Building with $engineRoot"
    $arguments = @(
        "-Target=\"$Target\"",
        "-Project=\"$ProjectPath\"",
        "-WaitMutex",
        "-FromMsBuild"
    )

    $start = Get-Date
    & $buildBat @arguments
    $exitCode = $LASTEXITCODE
    $duration = [int]((Get-Date) - $start).TotalSeconds

    $results += [PSCustomObject]@{
        engine_root = $engineRoot
        status = $(if ($exitCode -eq 0) { "passed" } else { "failed" })
        reason = $null
        exit_code = $exitCode
        duration_sec = $duration
    }
}

$results | ConvertTo-Json -Depth 5 | Out-File -FilePath $OutputJson -Encoding utf8
Write-Host "[Matrix] result saved to $OutputJson"
