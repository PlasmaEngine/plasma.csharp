param(
  [Parameter(Mandatory = $true)]
  [string]$PublishDirectory,

  [Parameter(Mandatory = $true)]
  [string]$DestinationRoot,

  [Parameter(Mandatory = $true)]
  [string]$RuntimeVersion
)

$ErrorActionPreference = "Stop"

$publishRoot = [System.IO.Path]::GetFullPath($PublishDirectory)
$destination = [System.IO.Path]::GetFullPath($DestinationRoot)
$destinationDriveRoot = [System.IO.Path]::GetPathRoot($destination)

if (-not (Test-Path -LiteralPath $publishRoot -PathType Container)) {
  throw "The published .NET runtime directory does not exist: $publishRoot"
}

if ([string]::IsNullOrWhiteSpace($destination) -or
    $destination -eq $destinationDriveRoot -or
    $destination.Length -lt ($destinationDriveRoot.Length + 4)) {
  throw "Refusing to prepare a .NET runtime at unsafe destination '$destination'."
}

$sharedRoot = Join-Path $destination "shared\Microsoft.NETCore.App\$RuntimeVersion"
$hostRoot = Join-Path $destination "host\fxr\$RuntimeVersion"

foreach ($target in @($sharedRoot, $hostRoot)) {
  $resolvedTarget = [System.IO.Path]::GetFullPath($target)
  if (-not $resolvedTarget.StartsWith($destination, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Runtime layout target escapes the destination root: $resolvedTarget"
  }

  if (Test-Path -LiteralPath $resolvedTarget) {
    Remove-Item -LiteralPath $resolvedTarget -Recurse -Force
  }
  New-Item -ItemType Directory -Path $resolvedTarget -Force | Out-Null
}

$excludedNames = @(
  "hostfxr.dll",
  "Plasma.ManagedHost.dll",
  "Plasma.ManagedHost.pdb",
  "Plasma.ManagedHost.deps.json",
  "Plasma.ManagedHost.runtimeconfig.json",
  "Plasma.ScriptCore.dll",
  "Plasma.ScriptCore.pdb",
  "Plasma.ScriptCore.xml"
)

Get-ChildItem -LiteralPath $publishRoot -File |
  Where-Object { $excludedNames -notcontains $_.Name } |
  ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $sharedRoot $_.Name) -Force
  }

$publishedHostFxr = Join-Path $publishRoot "hostfxr.dll"
if (-not (Test-Path -LiteralPath $publishedHostFxr -PathType Leaf)) {
  throw "Self-contained publish did not contain hostfxr.dll."
}
Copy-Item -LiteralPath $publishedHostFxr -Destination (Join-Path $hostRoot "hostfxr.dll") -Force

$nugetRoot = if ([string]::IsNullOrWhiteSpace($env:NUGET_PACKAGES)) {
  Join-Path ([Environment]::GetFolderPath("UserProfile")) ".nuget\packages"
} else {
  $env:NUGET_PACKAGES
}
$runtimePackageRoot = Join-Path $nugetRoot "microsoft.netcore.app.runtime.win-x64\$RuntimeVersion"

if ($RuntimeVersion -notmatch "^(\d+)\.(\d+)(?:\.|$)") {
  throw "The pinned .NET runtime version '$RuntimeVersion' does not contain a major and minor version."
}
$runtimeTargetFramework = "net$($Matches[1]).$($Matches[2])"
$frameworkMetadataRoot = Join-Path $runtimePackageRoot "runtimes\win-x64\lib\$runtimeTargetFramework"

foreach ($metadataName in @("Microsoft.NETCore.App.deps.json", "Microsoft.NETCore.App.runtimeconfig.json")) {
  $metadataSource = Join-Path $frameworkMetadataRoot $metadataName
  if (-not (Test-Path -LiteralPath $metadataSource -PathType Leaf)) {
    throw "The pinned runtime framework metadata is missing: $metadataSource"
  }
  Copy-Item -LiteralPath $metadataSource -Destination (Join-Path $sharedRoot $metadataName) -Force
}

foreach ($noticeName in @("LICENSE.TXT", "THIRD-PARTY-NOTICES.TXT")) {
  $noticeSource = Join-Path $runtimePackageRoot $noticeName
  if (-not (Test-Path -LiteralPath $noticeSource -PathType Leaf)) {
    throw "The pinned runtime package notice is missing: $noticeSource"
  }
  Copy-Item -LiteralPath $noticeSource -Destination (Join-Path $destination $noticeName) -Force
}

$destinationPrefix = $destination.TrimEnd("\", "/") + [System.IO.Path]::DirectorySeparatorChar
$inventoryFile = Join-Path $destination "runtime-inventory.json"
$inventory = Get-ChildItem -LiteralPath $destination -Recurse -File |
  Where-Object { $_.FullName -ne $inventoryFile } |
  Sort-Object FullName |
  ForEach-Object {
    [pscustomobject]@{
      path = $_.FullName.Substring($destinationPrefix.Length).Replace("\", "/")
      bytes = $_.Length
      sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }
  }

$inventory |
  ConvertTo-Json -Depth 3 |
  Set-Content -LiteralPath $inventoryFile -Encoding utf8
