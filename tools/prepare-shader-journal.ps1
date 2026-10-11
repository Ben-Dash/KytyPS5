param(
    [Parameter(Mandatory = $true)][string]$Journal,
    [Parameter(Mandatory = $true)][string]$RecordIndex,
    [Parameter(Mandatory = $true)][string]$CompilerSettings,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [int[]]$SelectRecord = @()
)
$ErrorActionPreference = 'Stop'
function Hash-Bytes([byte[]]$Bytes) {
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hasher.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $hasher.Dispose() }
}
$Journal = (Get-Item -LiteralPath $Journal).FullName
$RecordIndex = (Get-Item -LiteralPath $RecordIndex).FullName
$CompilerSettings = (Get-Item -LiteralPath $CompilerSettings).FullName
if ((Get-Item -LiteralPath $Journal).Length -gt 256MB) { throw 'Journal exceeds v8 file bound.' }
if ((Get-Item -LiteralPath $RecordIndex).Length -gt 16MB) { throw 'Record index exceeds bound.' }
if ((Get-Item -LiteralPath $CompilerSettings).Length -gt 1MB) { throw 'Compiler settings exceed bound.' }
$index = [IO.File]::ReadAllText($RecordIndex) | ConvertFrom-Json
$settings = [IO.File]::ReadAllText($CompilerSettings) | ConvertFrom-Json
if ($index.record_index_base -ne 0 -or $index.scope -ne 'v8_journal_success_permutation' -or $index.records.Count -gt 32768) {
    throw 'Expected the verified zero-based v8 record index.'
}
$bytes = [IO.File]::ReadAllBytes($Journal)
$journalHash = Hash-Bytes $bytes
if ($journalHash -ne $index.journal_sha256) { throw 'Journal does not match the frozen verified record index.' }
$stream = New-Object IO.MemoryStream(,$bytes)
$reader = New-Object IO.BinaryReader($stream)
$selected = New-Object 'System.Collections.Generic.List[object]'
try {
    if ([Text.Encoding]::ASCII.GetString($reader.ReadBytes(8)) -ne 'KYTYSHDR' -or $reader.ReadUInt32() -ne 8) {
        throw 'Only v8 KYTYSHDR journals are supported.'
    }
    $keyLength = $reader.ReadUInt32()
    if ($keyLength -gt 4096 -or $keyLength -gt $stream.Length - $stream.Position) { throw 'Invalid journal key length.' }
    $null = $reader.ReadBytes($keyLength)
    for ($ordinal = 0; $ordinal -lt $index.records.Count; $ordinal++) {
        if ($stream.Length - $stream.Position -lt 12) { throw 'Truncated record envelope.' }
        $row = $index.records[$ordinal]
        if ($row.index -ne $ordinal -or $row.envelope_offset -ne $stream.Position) { throw 'Record index/envelope offset mismatch.' }
        $length = $reader.ReadUInt32()
        $checksum = $reader.ReadUInt64()
        if ($length -eq 0 -or $length -gt 4MB -or $length -gt $stream.Length - $stream.Position -or
            $row.body_offset -ne $stream.Position -or $row.body_bytes -ne $length -or
            $row.body_xxh3 -ne ('{0:x16}' -f $checksum)) { throw 'Record framing/checksum metadata mismatch.' }
        $body = $reader.ReadBytes($length)
        if ((Hash-Bytes $body) -ne $row.body_sha256) { throw 'Record body does not match the verified index.' }
        if (-not $SelectRecord.Count -or $ordinal -in $SelectRecord) {
            $selected.Add([pscustomobject]@{ row = $row; body = $body })
        }
    }
    if ($stream.Position -ne $stream.Length) { throw 'Journal contains an unindexed or torn tail.' }
    foreach ($requested in $SelectRecord) {
        if ($requested -lt 0 -or $requested -ge $index.records.Count) { throw 'Requested record index is outside the journal.' }
    }
} finally { $reader.Dispose(); $stream.Dispose() }
$OutputDirectory = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory))
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Corpus destination must be new; existing captures are preserved.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
foreach ($item in $selected) {
    $row = $item.row
    $bodyName = 'record-' + $row.body_sha256 + '.bin'
    $bodyPath = Join-Path $OutputDirectory $bodyName
    if (-not (Test-Path -LiteralPath $bodyPath)) { [IO.File]::WriteAllBytes($bodyPath, $item.body) }
    $manifest = [ordered]@{
        schema_version = 3; kind = 'journal_record'; journal_version = 8
        stage = $row.stage; shader_hash = $row.shader_hash; code_file = $bodyName
        record_bytes = $row.body_bytes; record_xxh3_64 = $row.body_xxh3
        compiler_settings = $settings
        capture_attempt = [ordered]@{
            origin = 'verified_v8_journal'; record_index = $row.index
            journal_sha256 = $journalHash; body_sha256 = $row.body_sha256
            envelope_offset = $row.envelope_offset; body_offset = $row.body_offset
        }
    }
    $manifest | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath (Join-Path $OutputDirectory ('record-{0:D5}.json' -f $row.index)) -Encoding UTF8
}
Write-Output ('Prepared {0} record manifests from {1} verified indexed bodies; checksum is independently checked by the native worker.' -f $selected.Count, $index.records.Count)
Write-Output ('Corpus: ' + $OutputDirectory)
