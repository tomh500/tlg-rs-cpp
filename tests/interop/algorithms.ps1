param(
    [Parameter(Mandatory = $true)][string]$RustOracle,
    [string]$CppTool = "$PSScriptRoot/../../build/msvc/Release/tlg_tool.exe",
    [string]$OutputDirectory = "$PSScriptRoot/../../build/interop-latest"
)
$ErrorActionPreference = 'Stop'
$rust = (Resolve-Path -LiteralPath $RustOracle).Path
$cpp = (Resolve-Path -LiteralPath $CppTool).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$random = [Random]::new(12582917)
$checks = 0
$cases = 0
foreach ($shape in @(@(1,1), @(1,273), @(273,1), @(7,9), @(8,8), @(9,17), @(33,31), @(129,65))) {
    $width, $height = $shape
    foreach ($channel in @(1,3,4)) {
        $layout = switch ($channel) { 1 { 'gray' }; 3 { 'rgb' }; 4 { 'rgba' } }
        foreach ($pattern in @('random','periodic')) {
            $name = "${width}x${height}x${channel}-$pattern"
            $raw = [byte[]]::new($width * $height * $channel)
            $random.NextBytes($raw)
            if ($pattern -eq 'periodic') {
                for ($i = 0; $i -lt $raw.Length; ++$i) { $raw[$i] = [byte](($i % 19) * 13) }
            }
            $rawPath = Join-Path $output "$name.raw"
            [IO.File]::WriteAllBytes($rawPath, $raw)
            $expectedHash = (Get-FileHash -LiteralPath $rawPath).Hash
            foreach ($format in @(5,6)) {
                $rustEncoded = Join-Path $output "$name.rust.tlg$format"
                & $rust encode_raw $format $width $height $channel $rawPath $rustEncoded
                if ($LASTEXITCODE -ne 0) { throw "Rust encode $name TLG$format failed" }
                foreach ($decode in @('bulk','scalar')) {
                    $decoded = Join-Path $output "$name.$decode.raw"
                    & $cpp decode --slide-decode $decode $rustEncoded $decoded
                    if ($LASTEXITCODE -ne 0 -or (Get-FileHash -LiteralPath $decoded).Hash -ne $expectedHash) {
                        throw "C++ $decode decoding latest Rust $name TLG$format failed"
                    }
                    ++$checks
                }
                foreach ($parse in @('greedy','optimal')) {
                  foreach ($match in @('adaptive','primary')) {
                    foreach ($search in $(if ($format -eq 6) { @('factored','reference') } else { @('factored') })) {
                        $encoded = Join-Path $output "$name.$parse.$match.$search.tlg$format"
                        & $cpp encode --format $format --width $width --height $height --layout $layout --slide-parse $parse --slide-match $match --filter-search $search $rawPath $encoded
                        if ($LASTEXITCODE -ne 0) { throw "C++ $parse/$search encoding $name TLG$format failed" }
                        $null = & $rust verify $format $width $height $channel $encoded $rawPath
                        if ($LASTEXITCODE -ne 0) { throw "Latest Rust decoding C++ $parse/$search $name TLG$format failed" }
                        ++$checks
                    }
                  }
                }
                ++$cases
            }
        }
    }
}
Write-Host "PASS: $checks latest-Rust cross-language algorithm checks across $cases image/format cases."
