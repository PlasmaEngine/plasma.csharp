param(
  [Parameter(Mandatory = $true)]
  [string]$ProjectRoot,

  [Parameter(Mandatory = $true)]
  [string]$Configuration,

  [Parameter(Mandatory = $true)]
  [string]$BinaryRoot,

  [Parameter(Mandatory = $true)]
  [string]$RuntimePublishRoot,

  [Parameter(Mandatory = $true)]
  [string]$PrivateRuntimeRoot,

  [Parameter(Mandatory = $true)]
  [string]$RuntimeVersion
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repositoryRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
$managedRoot = Join-Path $repositoryRoot "Source\CSharpPlugin\Managed"
$hostProject = Join-Path $managedRoot "Plasma.ManagedHost\Plasma.ManagedHost.csproj"
$gameProject = Join-Path $managedRoot "Plasma.ManagedM0Game\Plasma.ManagedM0Game.csproj"
$gameOutput = Join-Path ([System.IO.Path]::GetFullPath($BinaryRoot)) "M0Game"
$prepareScript = Join-Path $repositoryRoot "Scripts\prepare-dotnet-runtime.ps1"

function Invoke-DotNet {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Description,

    [Parameter(Mandatory = $true)]
    [string[]]$Arguments
  )

  Write-Host $Description
  & dotnet @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "$Description failed with exit code $LASTEXITCODE."
  }
}

Invoke-DotNet -Description "Building Plasma managed host..." -Arguments @(
  "build",
  $hostProject,
  "--nologo",
  "--configuration",
  $Configuration,
  "--output",
  ([System.IO.Path]::GetFullPath($BinaryRoot))
)

Invoke-DotNet -Description "Building Plasma managed M0 game..." -Arguments @(
  "build",
  $gameProject,
  "--nologo",
  "--configuration",
  $Configuration,
  "--output",
  $gameOutput
)

Invoke-DotNet -Description "Publishing pinned private .NET runtime..." -Arguments @(
  "publish",
  $hostProject,
  "--nologo",
  "--configuration",
  $Configuration,
  "--runtime",
  "win-x64",
  "--self-contained",
  "true",
  "-p:PublishSingleFile=false",
  "-p:RuntimeFrameworkVersion=$RuntimeVersion",
  "--output",
  ([System.IO.Path]::GetFullPath($RuntimePublishRoot))
)

Write-Host "Preparing private .NET runtime layout..."
& $prepareScript `
  -PublishDirectory ([System.IO.Path]::GetFullPath($RuntimePublishRoot)) `
  -DestinationRoot ([System.IO.Path]::GetFullPath($PrivateRuntimeRoot)) `
  -RuntimeVersion $RuntimeVersion
