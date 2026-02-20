Param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = Resolve-Path (Join-Path $ScriptDir "..")
$RepoRoot = Resolve-Path (Join-Path $RootDir "..")

$ConfigPath = $env:UE_MCP_CONFIG
if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
    $ConfigPath = Join-Path $RootDir "configs/config.yaml"
}
if (!(Test-Path $ConfigPath)) {
    $ExampleConfigPath = Join-Path $RootDir "configs/config.example.yaml"
    if (Test-Path $ExampleConfigPath) {
        $ConfigPath = $ExampleConfigPath
    }
}

if ([string]::IsNullOrWhiteSpace($env:UE_MCP_CONNECTION_FILE)) {
    if (![string]::IsNullOrWhiteSpace($env:UE_MCP_PROJECT_ROOT)) {
        $env:UE_MCP_CONNECTION_FILE = Join-Path $env:UE_MCP_PROJECT_ROOT "Saved/UnrealMCP/connection.json"
    }
    else {
        $uproject = Get-ChildItem -Path $RepoRoot -Filter *.uproject -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $uproject) {
            $env:UE_MCP_PROJECT_ROOT = $uproject.Directory.FullName
            $env:UE_MCP_CONNECTION_FILE = Join-Path $env:UE_MCP_PROJECT_ROOT "Saved/UnrealMCP/connection.json"
        }
    }
}

$PythonBin = $env:UE_MCP_PYTHON
if ([string]::IsNullOrWhiteSpace($PythonBin)) {
    $VenvPython = Join-Path $RootDir ".venv/Scripts/python.exe"
    if (Test-Path $VenvPython) {
        $PythonBin = $VenvPython
    }
    else {
        $PythonBin = "python"
    }
}

$SourcePath = Join-Path $RootDir "src"
if ([string]::IsNullOrWhiteSpace($env:PYTHONPATH)) {
    $env:PYTHONPATH = $SourcePath
}
else {
    $env:PYTHONPATH = "$SourcePath;$($env:PYTHONPATH)"
}

& $PythonBin -m mcp_server.mcp_stdio --config $ConfigPath @ExtraArgs
exit $LASTEXITCODE
