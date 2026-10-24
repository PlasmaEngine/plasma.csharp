param(
  [Parameter(Mandatory = $true)][string]$ProjectRoot,
  [Parameter(Mandatory = $true)][string]$OutputDirectory,
  [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
# The tools share ScriptCore's intermediate files. Restore and build them sequentially.
foreach ($tool in @('Plasma.ScriptInspector', 'Plasma.ScriptCompiler')) {
  $project = Join-Path $ProjectRoot "Source/EditorManaged/$tool/$tool.csproj"
  & dotnet build $project --nologo --configuration $Configuration --output $OutputDirectory
  if ($LASTEXITCODE -ne 0) { throw "Building $tool failed ($LASTEXITCODE)." }
}
