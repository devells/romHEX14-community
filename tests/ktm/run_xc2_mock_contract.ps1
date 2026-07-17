[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [AllowEmptyString()]
    [string]$BaseUrl,

    [string]$ProbePath = (Join-Path $PSScriptRoot '..\..\build-test\xc2_contract_probe.exe'),

    [ValidateRange(100, 120000)]
    [int]$TimeoutMs = 30000
)

try {
    $resolved = @(Resolve-Path -LiteralPath $ProbePath -ErrorAction Stop)
    if ($resolved.Count -ne 1) {
        exit 2
    }
    $resolvedProbePath = $resolved[0].ProviderPath
    if (-not (Test-Path -LiteralPath $resolvedProbePath -PathType Leaf)) {
        exit 2
    }
}
catch {
    exit 2
}

$probeArguments = @(
    '--base-url',
    $BaseUrl,
    '--timeout-ms',
    [string]$TimeoutMs
)
$global:LASTEXITCODE = 2
& $resolvedProbePath @probeArguments
$probeExitCode = $LASTEXITCODE
exit [int]$probeExitCode
