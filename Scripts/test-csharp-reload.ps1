param(
  [Parameter(Mandatory = $true)][string]$SampleRoot,
  [string]$SdkRoot = $env:PLASMA_SDK_ROOT,
  [string]$OutputRoot
)
$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path $PSScriptRoot -Parent
$SampleRoot = (Resolve-Path -LiteralPath $SampleRoot).Path
$SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path
if (-not $OutputRoot) { $OutputRoot = Join-Path $packageRoot ('Intermediate/ReloadTests/' + [Guid]::NewGuid().ToString('N')) }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw 'Use a fresh output directory; existing test results are preserved.' }
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$fixture = Join-Path $OutputRoot 'Fixture'
$testProject = Join-Path $packageRoot 'Source/EditorManaged/Plasma.ScriptCompiler.Tests/Plasma.ScriptCompiler.Tests.csproj'
$previousModifiableAssemblies = $env:DOTNET_MODIFIABLE_ASSEMBLIES
try {
  $env:DOTNET_MODIFIABLE_ASSEMBLIES = 'debug'
  Push-Location $packageRoot
  try {
    & dotnet run --project $testProject -c Release -- $SampleRoot $fixture
    if ($LASTEXITCODE -ne 0) { throw 'Managed BrickBreaker reload integration failed.' }
  } finally { Pop-Location }
} finally { $env:DOTNET_MODIFIABLE_ASSEMBLIES = $previousModifiableAssemblies }

$runtime = Join-Path $OutputRoot 'Runtime'
New-Item -ItemType Directory -Path $runtime -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $SdkRoot 'Binaries/Windows_Development') -File |
  Where-Object { $_.Extension -eq '.dll' } | Copy-Item -Destination $runtime
$packageBinaries = Join-Path $packageRoot 'Bin/Windows_x64_Development'
Get-ChildItem -LiteralPath $packageBinaries -File |
  Where-Object { $_.Extension -eq '.dll' -or $_.Name -eq 'CSharpRuntimeTests.exe' } | Copy-Item -Destination $runtime
foreach ($folder in @('CSharp', 'DotNet')) { Copy-Item -LiteralPath (Join-Path $packageBinaries $folder) -Destination $runtime -Recurse }
Set-Content -LiteralPath (Join-Path $runtime 'plSdkRoot.txt') -Value ([IO.Path]::GetRelativePath($runtime, $SdkRoot)) -NoNewline
$log = Join-Path $OutputRoot 'Native.log'
$errorLog = Join-Path $OutputRoot 'Native.errors.log'
$process = Start-Process -FilePath (Join-Path $runtime 'CSharpRuntimeTests.exe') -ArgumentList '-fixture', ('"' + $fixture + '"') -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $errorLog
if (-not $process.WaitForExit(120000)) { $process.Kill(); throw "Native reload test timed out. See $OutputRoot" }
Get-Content -LiteralPath $log
Get-Content -LiteralPath $errorLog
if ($process.ExitCode -ne 0) { throw "Native C# reload integration failed ($($process.ExitCode)). See $OutputRoot" }
Write-Host "Reload integration passed. Results: $OutputRoot"
