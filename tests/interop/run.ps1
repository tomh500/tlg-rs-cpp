param(
    [Parameter(Mandatory = $true)][string]$RustTree,
    [string]$CppTool = "$PSScriptRoot/../../build/msvc/Release/tlg_tool.exe",
    [string]$OutputDirectory = "$PSScriptRoot/../../build/interop",
    [string]$Cargo = "cargo"
)
$ErrorActionPreference = 'Stop'
$rustRoot = (Resolve-Path -LiteralPath $RustTree).Path
$toolPath = (Resolve-Path -LiteralPath $CppTool).Path
$fixtureRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../fixtures").Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
New-Item -ItemType Directory -Force -Path "$rustRoot/examples" | Out-Null
Copy-Item -LiteralPath "$PSScriptRoot/rust_oracle.rs" -Destination "$rustRoot/examples/interop.rs"
& $Cargo build --manifest-path "$rustRoot/Cargo.toml" --release --no-default-features --example interop
if ($LASTEXITCODE -ne 0) { throw 'Rust oracle build failed' }
$oraclePath = Join-Path $rustRoot 'target/release/examples/interop.exe'
if (-not (Test-Path -LiteralPath $oraclePath)) {
    $oraclePath = Join-Path $rustRoot 'target/release/examples/interop'
}
$cases = @(
    @('gray_1x1_zero', 1, 1, 'gray', 1),
    @('gray_19x13_random', 19, 13, 'gray', 1),
    @('rgb_9x11_gradient', 9, 11, 'rgb', 3),
    @('rgb_32x24_flat', 32, 24, 'rgb', 3),
    @('rgba_17x9_random', 17, 9, 'rgba', 4),
    @('rgba_32x24_gradient', 32, 24, 'rgba', 4)
)
$count = 0
foreach ($case in $cases) {
    $name, $width, $height, $layout, $channels = $case
    $rawPath = Join-Path $fixtureRoot "$name.raw"
    foreach ($format in @(5, 6)) {
        $cppEncoded = Join-Path $outputRoot "$name.cpp.tlg$format"
        & $toolPath encode --format $format --width $width --height $height --layout $layout $rawPath $cppEncoded
        if ($LASTEXITCODE -ne 0) { throw "C++ encode failed for $name TLG$format" }
        & $oraclePath verify $format $width $height $channels $cppEncoded $rawPath
        if ($LASTEXITCODE -ne 0) { throw "Rust decoding C++ failed for $name TLG$format" }
        # The checked-in fixtures are intentionally old. This direction must
        # exercise the Rust tree being tested, not quietly replay those fixtures.
        $rustEncoded = Join-Path $outputRoot "$name.rust.tlg$format"
        & $oraclePath encode_raw $format $width $height $channels $rawPath $rustEncoded
        if ($LASTEXITCODE -ne 0) { throw "Rust encode failed for $name TLG$format" }
        $cppDecoded = Join-Path $outputRoot "$name.cpp.decoded$format.raw"
        & $toolPath decode $rustEncoded $cppDecoded
        if ($LASTEXITCODE -ne 0) { throw "C++ decoding Rust failed for $name TLG$format" }
        $expectedHash = (Get-FileHash -LiteralPath $rawPath -Algorithm SHA256).Hash
        $actualHash = (Get-FileHash -LiteralPath $cppDecoded -Algorithm SHA256).Hash
        if ($expectedHash -ne $actualHash) { throw "C++ pixel mismatch for $name TLG$format" }
        $count += 2
    }
}
Write-Host "PASS: $count bidirectional codec checks; SDS metadata is independently checked by tlg_tests."
