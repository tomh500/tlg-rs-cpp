param(
    [Parameter(Mandatory = $true)][string]$RustTree,
    [string]$CppTool = "$PSScriptRoot/../../build/msvc/Release/tlg_tool.exe",
    [string]$OutputDirectory = "$PSScriptRoot/../../build/image-interop",
    [string]$Cargo = 'cargo'
)
$ErrorActionPreference = 'Stop'
$rustRoot = (Resolve-Path -LiteralPath $RustTree).Path
$cpp = (Resolve-Path -LiteralPath $CppTool).Path
$fixtures = (Resolve-Path -LiteralPath "$PSScriptRoot/../fixtures").Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$manifestPath = Join-Path $rustRoot 'Cargo.toml'
$original = [IO.File]::ReadAllText($manifestPath)
# Only the disposable test tree's codec-feature selection changes. Keep the
# actual upstream TLG library source untouched; restore the manifest afterwards.
$jpegFeatures = 'features = ["jpeg"]'
$extendedFeatures = 'features = ["jpeg", "png", "bmp", "tiff", "qoi", "tga", "ff", "pnm", "dds"]'
if (-not $original.Contains($jpegFeatures) -and -not $original.Contains($extendedFeatures)) {
    throw 'Expected the pinned v1.0.0 JPEG test dependency in the temporary Rust tree'
}
try {
    [IO.File]::WriteAllText($manifestPath, $original.Replace($jpegFeatures, $extendedFeatures))
    New-Item -ItemType Directory -Force -Path "$rustRoot/examples" | Out-Null
    Copy-Item -LiteralPath "$PSScriptRoot/rust_oracle.rs" -Destination "$rustRoot/examples/interop_images.rs"
    & $Cargo build --manifest-path $manifestPath --release --no-default-features --features image --example interop_images --locked
    if ($LASTEXITCODE -ne 0) { throw 'Rust image oracle build failed' }
    $oracle = Join-Path $rustRoot 'target/release/examples/interop_images.exe'
    if (-not (Test-Path -LiteralPath $oracle)) { $oracle = Join-Path $rustRoot 'target/release/examples/interop_images' }
    $helpText = & $cpp --help
    if ($LASTEXITCODE -ne 0) { throw 'C++ tool is unavailable' }
    $count = 0
    foreach ($case in @(@('gray_19x13_random',19,13,1), @('rgb_9x11_gradient',9,11,3), @('rgba_17x9_random',17,9,4))) {
        $name, $width, $height, $channels = $case
        $extensions = @('pam','qoi','tga','ff')
        if ($helpText -match 'WIC image backend') { $extensions += @('png','bmp','tiff') }
        foreach ($extension in $extensions) {
            $imagePath = Join-Path $output "$name.$extension"
            & $cpp decode (Join-Path $fixtures "$name.tlg6") $imagePath
            if ($LASTEXITCODE -ne 0) { throw "C++ $extension output failed for $name" }
            & $oracle verify_image $width $height $channels $imagePath (Join-Path $fixtures "$name.raw")
            if ($LASTEXITCODE -ne 0) { throw "Independent Rust image reader disagrees for $name $extension" }
            ++$count
        }
    }
    Write-Host "PASS: $count C++ image outputs decoded pixel-exactly by independent image-rs readers."
} finally {
    [IO.File]::WriteAllText($manifestPath, $original)
}
