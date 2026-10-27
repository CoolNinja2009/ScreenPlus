$ErrorActionPreference = 'Stop'

$repoRoot = (& git rev-parse --show-toplevel).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to find the repository root.'
}

$trackedPaths = @(& git -C $repoRoot ls-files)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to list tracked files.'
}

$vendoredPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
for ($offset = 0; $offset -lt $trackedPaths.Count; $offset += 100) {
    $end = [Math]::Min($offset + 99, $trackedPaths.Count - 1)
    $batch = @($trackedPaths[$offset..$end])
    $attributes = @(& git -C $repoRoot check-attr linguist-vendored -- $batch)
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to read linguist-vendored attributes.'
    }

    foreach ($attribute in $attributes) {
        if ($attribute -match '^(.+): linguist-vendored: true$') {
            [void]$vendoredPaths.Add($Matches[1])
        }
    }
}

$sourceExtensions = @('.c', '.h', '.js', '.css', '.html', '.frag', '.hlsl', '.sass', '.sh', '.ps1', '.cmd', '.nsi', '.rc')
$lineCount = 0
foreach ($relativePath in $trackedPaths) {
    if ($vendoredPaths.Contains($relativePath) -or [IO.Path]::GetExtension($relativePath) -notin $sourceExtensions) {
        continue
    }

    $filePath = Join-Path $repoRoot $relativePath
    if (Test-Path -LiteralPath $filePath -PathType Leaf) {
        $lineCount += @(Get-Content -LiteralPath $filePath).Count
    }
}

$badge = @{
    schemaVersion = 1
    label = 'code lines'
    message = $lineCount.ToString('N0', [Globalization.CultureInfo]::InvariantCulture)
    color = 'blue'
}
$badgePath = Join-Path $repoRoot '.github\code-lines.json'
$json = ($badge | ConvertTo-Json -Compress) + [Environment]::NewLine
[IO.File]::WriteAllText($badgePath, $json, [Text.UTF8Encoding]::new($false))
Write-Output "Counted $lineCount code lines."
