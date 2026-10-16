param(
    [Parameter(Mandatory = $true)][string]$RustOracle,
    [string]$CppBenchmark = "$PSScriptRoot/../build/msvc/Release/tlg_benchmark.exe",
    [int]$Width = 512,
    [int]$Height = 512,
    [int]$Rounds = 30,
    [int]$Samples = 5,
    [string]$OutputDirectory = "$PSScriptRoot/../build/benchmark",
    [ValidateSet('greedy', 'optimal')][string]$SlideParse = 'greedy',
    [ValidateSet('factored', 'reference', 'refined', 'compact')][string]$FilterSearch = 'factored',
    [ValidateSet('bulk', 'scalar')][string]$SlideDecode = 'bulk',
    [string]$RawFile = '',
    [ValidateSet(1, 3, 4)][int]$Channels = 4,
    [switch]$RustCodecOnly,
    [ValidateSet('adaptive', 'primary')][string]$SlideMatch = 'adaptive'
)
$ErrorActionPreference = 'Stop'
if ($Width -lt 1 -or $Height -lt 1 -or $Rounds -lt 1 -or $Samples -lt 1) { throw 'Width, height, rounds, and samples must be positive' }
$cppPath = (Resolve-Path -LiteralPath $CppBenchmark).Path
$rustPath = (Resolve-Path -LiteralPath $RustOracle).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
# Both executables perform one untimed encode/decode first for every row.
# Alternate their order; summarize the median of independent process means.
$cppData = @()
$rustData = @()
for ($sample = 1; $sample -le $Samples; ++$sample) {
    foreach ($implementation in $(if ($sample % 2 -eq 1) { @('cpp', 'rust') } else { @('rust', 'cpp') })) {
        if ($implementation -eq 'cpp') {
            $arguments = @($Width, $Height, $Rounds, $SlideParse, $FilterSearch, $SlideDecode)
            if ($RawFile) { $arguments += @((Resolve-Path -LiteralPath $RawFile).Path, $Channels, $SlideMatch) }
            else { $arguments += @('-', 4, $SlideMatch) }
            $rows = & $cppPath @arguments
        } elseif ($RawFile) {
            $command = if ($RustCodecOnly) { 'benchmark_raw_codec' } else { 'benchmark_raw' }
            $rows = & $rustPath $command $Width $Height $Channels $Rounds (Resolve-Path -LiteralPath $RawFile).Path
        } else {
            $command = if ($RustCodecOnly) { 'benchmark_codec' } else { 'benchmark' }
            $rows = & $rustPath $command $Width $Height $Rounds
        }
        if ($LASTEXITCODE -ne 0) { throw "$implementation benchmark failed" }
        $data = $rows | ConvertFrom-Csv
        $expectedRows = if ($RawFile) { 2 } else { 6 }
        if (@($data).Count -ne $expectedRows) { throw "$implementation emitted an incomplete workload table" }
        foreach ($row in $data) {
            foreach ($field in @('encode_ms', 'decode_ms')) {
                $duration = [double]::Parse($row.$field, [Globalization.CultureInfo]::InvariantCulture)
                if ($duration -le 0 -or [double]::IsNaN($duration) -or [double]::IsInfinity($duration)) {
                    throw "$implementation $field must be finite and positive; rebuild the benchmark with full timing precision or increase rounds"
                }
                $row.$field = $duration
            }
        }
        $data | Add-Member -NotePropertyName sample -NotePropertyValue $sample
        if ($implementation -eq 'cpp') { $cppData += $data }
        else { $rustData += $data }
    }
}
$cppData | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "$outputRoot/cpp.csv"
$rustData | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "$outputRoot/rust.csv"
function Median($Values) {
    $sorted = @($Values | ForEach-Object { [double]$_ } | Sort-Object)
    $middle = [int][math]::Floor($sorted.Length / 2)
    if ($sorted.Length % 2 -eq 1) { return $sorted[$middle] }
    return ($sorted[$middle - 1] + $sorted[$middle]) / 2
}
$summary = @(foreach ($key in $cppData | Select-Object format,pattern -Unique) {
    $cpp = @($cppData | Where-Object { $_.format -eq $key.format -and $_.pattern -eq $key.pattern })
    $rust = @($rustData | Where-Object { $_.format -eq $key.format -and $_.pattern -eq $key.pattern })
    if ($cpp.Count -ne $Samples -or $rust.Count -ne $Samples) { throw 'C++ and Rust workload samples do not match' }
    if (@($cpp.encoded_bytes | Select-Object -Unique).Count -ne 1 -or
        @($rust.encoded_bytes | Select-Object -Unique).Count -ne 1) { throw 'Encoded byte counts changed between samples' }
    $cppEncode = Median $cpp.encode_ms
    $rustEncode = Median $rust.encode_ms
    $cppDecode = Median $cpp.decode_ms
    $rustDecode = Median $rust.decode_ms
    [PSCustomObject]@{
        Format = "TLG$($key.format)"
        Pattern = $key.pattern
        Samples = $Samples
        Rounds = $Rounds
        SlideParse = $SlideParse
        FilterSearch = $FilterSearch
        SlideDecode = $SlideDecode
        SlideMatch = $SlideMatch
        RustMeasurement = $(if ($RustCodecOnly) { 'clone excluded' } else { 'public API' })
        CppBytes = [long]$cpp[0].encoded_bytes
        RustBytes = [long]$rust[0].encoded_bytes
        CppEncodeMs = $cppEncode
        RustEncodeMs = $rustEncode
        EncodeSpeedup = [math]::Round($rustEncode / $cppEncode, 2)
        CppDecodeMs = $cppDecode
        RustDecodeMs = $rustDecode
        DecodeSpeedup = [math]::Round($rustDecode / $cppDecode, 2)
    }
})
$summary | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "$outputRoot/summary.csv"
$summary
