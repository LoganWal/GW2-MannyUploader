param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$TargetDirectory = 'target',
    [string]$Toolchain = 'stable'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = Split-Path -Parent $PSScriptRoot
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$targetRoot = [IO.Path]::GetFullPath($TargetDirectory)
if (Test-Path -LiteralPath $outputRoot) {
    throw 'Wine test staging requires a fresh output directory'
}
New-Item -ItemType Directory -Path $outputRoot | Out-Null

Push-Location $repository
try {
    foreach ($configuration in @('debug', 'release')) {
        $arguments = @("+$Toolchain", 'test', '--locked', '--workspace', '--no-run',
            '--message-format=json', '--target', 'x86_64-pc-windows-msvc', '--target-dir', $targetRoot)
        if ($configuration -eq 'release') { $arguments += '--release' }
        $messages = & cargo @arguments
        if ($LASTEXITCODE -ne 0) { throw 'Cargo could not prepare the Wine test executables' }
        $artifacts = @($messages | ForEach-Object { $_ | ConvertFrom-Json } |
            Where-Object { $_.reason -eq 'compiler-artifact' -and $_.profile.test -and $_.executable })
        if ($artifacts.Count -eq 0) { throw 'Cargo reported no test executables' }
        $destination = Join-Path $outputRoot $configuration
        New-Item -ItemType Directory -Path $destination | Out-Null
        $expectedRoot = [IO.Path]::GetFullPath((Join-Path $targetRoot "x86_64-pc-windows-msvc/$configuration/deps"))
        $names = @()
        foreach ($artifact in $artifacts) {
            $source = [IO.Path]::GetFullPath($artifact.executable)
            if ((Split-Path -Parent $source) -ne $expectedRoot -or
                [IO.Path]::GetExtension($source) -ne '.exe') {
                throw 'Cargo reported a test executable outside the selected MSVC profile'
            }
            $name = Split-Path -Leaf $source
            if ($name -in $names) { throw 'Cargo reported duplicate test executable names' }
            Copy-Item -LiteralPath $source -Destination (Join-Path $destination $name)
            $names += $name
        }
        @{ schema_version = 1; configuration = $configuration; executables = @($names | Sort-Object) } |
            ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $destination 'manifest.json') -Encoding UTF8
        Write-Output "Staged $($names.Count) $configuration MSVC test executables for Wine"
    }
}
finally {
    Pop-Location
}
