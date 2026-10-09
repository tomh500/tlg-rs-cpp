param(
    [Parameter(Mandatory = $true)][string]$RustOracle,
    [string]$CppBenchmark = "$PSScriptRoot/../build/msvc/Release/tlg_benchmark.exe",
    [int]$Width = 512,
    [int]$Height = 512,
    [int]$Rounds = 30,
    [int]$Samples = 5,
    [string]$OutputDirectory = "$PSScriptRoot/../build/benchmark"
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
        if ($implementation -eq 'cpp') { $rows = & $cppPath $Width $Height $Rounds }
        else { $rows = & $rustPath benchmark $Width $Height $Rounds }
        if ($LASTEXITCODE -ne 0) { throw "$implementation benchmark failed" }
        $data = $rows | ConvertFrom-Csv
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
foreach ($key in $cppData | Select-Object format,pattern -Unique) {
    $cpp = @($cppData | Where-Object { $_.format -eq $key.format -and $_.pattern -eq $key.pattern })
    $rust = @($rustData | Where-Object { $_.format -eq $key.format -and $_.pattern -eq $key.pattern })
    $cppEncode = Median $cpp.encode_ms
    $rustEncode = Median $rust.encode_ms
    $cppDecode = Median $cpp.decode_ms
    $rustDecode = Median $rust.decode_ms
    [PSCustomObject]@{
        Format = "TLG$($key.format)"
        Pattern = $key.pattern
        Samples = $Samples
        Rounds = $Rounds
        CppBytes = [long]$cpp[0].encoded_bytes
        RustBytes = [long]$rust[0].encoded_bytes
        CppEncodeMs = $cppEncode
        RustEncodeMs = $rustEncode
        EncodeSpeedup = [math]::Round($rustEncode / $cppEncode, 2)
        CppDecodeMs = $cppDecode
        RustDecodeMs = $rustDecode
        DecodeSpeedup = [math]::Round($rustDecode / $cppDecode, 2)
    }
}
