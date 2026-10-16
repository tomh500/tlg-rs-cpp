param(
    [Parameter(Mandatory = $true)][string]$RustTree,
    [string]$Cargo = 'cargo'
)
$ErrorActionPreference = 'Stop'
$rustRoot = (Resolve-Path -LiteralPath $RustTree).Path
New-Item -ItemType Directory -Force -Path "$rustRoot/examples" | Out-Null
Copy-Item -LiteralPath "$PSScriptRoot/rust_oracle.rs" -Destination "$rustRoot/examples/interop.rs"
& $Cargo build --manifest-path "$rustRoot/Cargo.toml" --no-default-features --example interop --locked
if ($LASTEXITCODE -ne 0) { throw 'Upstream Debug oracle build failed' }
$oracle = Join-Path $rustRoot 'target/debug/examples/interop.exe'
if (-not (Test-Path -LiteralPath $oracle)) { $oracle = Join-Path $rustRoot 'target/debug/examples/interop' }
$details = & $oracle check_v110 2>&1
$exitCode = $LASTEXITCODE
if ($exitCode -eq 0) {
    Write-Host 'Upstream Debug API probe passed on this toolchain.'
} elseif ($exitCode -eq 101 -and ($details -join "`n") -match 'attempt to add with overflow' -and
    ($details -join "`n") -match 'src[\\/]tlg6[\\/]decode.rs:323:') {
    # This is a diagnostic observation of the unmodified upstream, not a claim
    # that its Debug decoder passed. C++ must independently pass its own tests.
    Write-Host 'KNOWN UPSTREAM FAILURE: v1.1.0 Debug packed_bytes_add still overflows in the ordinary addition at src/tlg6/decode.rs:323.'
    $details | Write-Host
} else {
    $details | Write-Host
    throw "Unexpected upstream Debug failure (exit $exitCode)"
}
# The known panic was handled above. Do not leak its native exit code into
# callers such as GitHub Actions' PowerShell wrapper.
exit 0
