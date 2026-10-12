param(
    [string]$CppTool = "$PSScriptRoot/../../build/avif/Release/tlg_tool.exe",
    [string]$Ffmpeg = 'ffmpeg',
    [string]$Python = 'python',
    [string]$OutputDirectory = "$PSScriptRoot/../../build/avif-interop"
)
$ErrorActionPreference = 'Stop'
$cpp = (Resolve-Path -LiteralPath $CppTool).Path
$fixtures = (Resolve-Path -LiteralPath "$PSScriptRoot/../fixtures").Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
foreach ($case in @(@('gray_19x13_random','gray'), @('rgb_9x11_gradient','rgb24'), @('rgba_17x9_random','rgba'))) {
    $name, $pixels = $case
    $encoded = Join-Path $output "$name.avif"
    $decoded = Join-Path $output "$name.raw"
    & $cpp decode (Join-Path $fixtures "$name.tlg6") $encoded
    if ($LASTEXITCODE -ne 0) { throw "C++ AVIF encode failed for $name" }
    & $Ffmpeg -v error -y -i $encoded -frames:v 1 -f rawvideo -pix_fmt $pixels $decoded
    if ($LASTEXITCODE -ne 0) { throw "Independent FFmpeg AVIF decode failed for $name" }
    $expected = Join-Path $fixtures "$name.raw"
    # This FFmpeg demuxer exposes AVIF alpha as a separate stream rather than
    # combining it with RGB. Check its color plane, then let Pillow independently
    # verify the complete auxiliary-alpha image and every transparent RGB byte.
    $check = @'
import sys
from pathlib import Path
from PIL import Image, features
assert features.check('avif'), 'Pillow must include AVIF support'
encoded, decoded, expected, fmt = sys.argv[1:]
a, b = Path(expected).read_bytes(), Path(decoded).read_bytes()
c = {'gray': 1, 'rgb24': 3, 'rgba': 4}[fmt]
assert len(a) == len(b)
if c == 4:
    assert all(a[i] == b[i] for i in range(len(a)) if i % 4 != 3), 'FFmpeg RGB plane mismatch'
else:
    assert a == b, 'FFmpeg plane mismatch'
rgba = bytearray()
for i in range(0, len(a), c):
    p = a[i:i+c]
    rgba.extend((p[0], p[0], p[0], 255) if c == 1 else (*p, 255) if c == 3 else p)
assert Image.open(encoded).convert('RGBA').tobytes() == rgba, 'Pillow RGBA image mismatch'
'@
    & $Python -c $check $encoded $decoded $expected $pixels
    if ($LASTEXITCODE -ne 0) { throw "Independent AVIF readers disagreed for $name" }
}
Write-Host 'PASS: independent AVIF color decoding (FFmpeg) and complete Gray/RGB/RGBA decoding (Pillow).'
