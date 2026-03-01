param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ArgsForward
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Resolve-Path (Join-Path $ScriptDir "..")
$VenvPython = Join-Path $ProjectRoot ".venv\\Scripts\\python.exe"

if (Test-Path $VenvPython) {
    $PythonExec = $VenvPython
} else {
    $PythonExec = "python"
}

Push-Location $ProjectRoot
try {
    & $PythonExec (Join-Path $ScriptDir "controlrig_e2e_runner.py") --python-executable $PythonExec @ArgsForward
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
