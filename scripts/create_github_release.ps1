$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$originUrl = (git -C $repoRoot remote get-url origin).Trim()
if ($originUrl -match 'github\.com[:/](.+?)(?:\.git)?$') {
    $repoSlug = $Matches[1]
}
else {
    throw "Failed to parse GitHub repo slug from origin URL: $originUrl"
}
$tag = 'v0.1.0'
$releaseName = 'v0.1.0'
$releaseNotes = @'
Initial public release.

- Unreal MCP plugin source
- Python MCP server
- Release bundles for internal installation
'@

$assets = @(
    Join-Path $repoRoot 'release-assets\ue5-mcp-plugin-v0.1.0.zip'
    Join-Path $repoRoot 'release-assets\mcp_server-v0.1.0.zip'
)

foreach ($asset in $assets) {
    if (-not (Test-Path $asset)) {
        throw "Release asset not found: $asset"
    }
}

$credInput = @(
    'protocol=https'
    'host=github.com'
    "path=$repoSlug"
    ''
) -join "`n"

$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$credRaw = $credInput | git credential-manager get 2>$null
$ErrorActionPreference = $prevEap

$tokenLine = $credRaw | Where-Object { $_ -like 'password=*' } | Select-Object -First 1
if (-not $tokenLine) {
    throw 'Failed to resolve GitHub token from git-credential-manager.'
}
$token = $tokenLine.Substring(9)

$headers = @{
    Authorization = "Bearer $token"
    Accept = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
    'User-Agent' = 'gamedevmcp-release-script'
}

$apiBase = "https://api.github.com/repos/$repoSlug"

$release = $null
try {
    $release = Invoke-RestMethod -Method Get -Uri "$apiBase/releases/tags/$tag" -Headers $headers -ErrorAction Stop
    Write-Host "Found existing release for tag $tag (id=$($release.id))."
}
catch {
    $status = $null
    if ($_.Exception.Response -and $_.Exception.Response.StatusCode) {
        $status = [int]$_.Exception.Response.StatusCode
    }
    if ($status -ne 404) {
        throw
    }

    $payload = @{
        tag_name = $tag
        target_commitish = 'main'
        name = $releaseName
        body = $releaseNotes
        draft = $false
        prerelease = $false
        generate_release_notes = $false
    } | ConvertTo-Json -Depth 5

    $release = Invoke-RestMethod -Method Post -Uri "$apiBase/releases" -Headers $headers -ContentType 'application/json' -Body $payload
    Write-Host "Created release for tag $tag (id=$($release.id))."
}

$release = Invoke-RestMethod -Method Get -Uri "$apiBase/releases/$($release.id)" -Headers $headers
$uploadBase = $release.upload_url.ToString().Replace('{?name,label}', '')

foreach ($assetPath in $assets) {
    $assetName = [System.IO.Path]::GetFileName($assetPath)
    $existing = $release.assets | Where-Object { $_.name -eq $assetName } | Select-Object -First 1
    if ($existing) {
        Invoke-RestMethod -Method Delete -Uri "$apiBase/releases/assets/$($existing.id)" -Headers $headers | Out-Null
        Write-Host "Deleted existing asset: $assetName"
    }

    $uploadUri = "$($uploadBase)?name=$([uri]::EscapeDataString($assetName))"
    Invoke-WebRequest -Method Post -Uri $uploadUri -Headers $headers -ContentType 'application/zip' -InFile $assetPath | Out-Null
    Write-Host "Uploaded asset: $assetName"

    $release = Invoke-RestMethod -Method Get -Uri "$apiBase/releases/$($release.id)" -Headers $headers
}

$assetNames = ($release.assets | ForEach-Object { $_.name }) -join ', '
Write-Host "Release URL: $($release.html_url)"
Write-Host "Assets: $assetNames"
