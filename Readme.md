# plasma.csharp

C# scripting for PlasmaEngine, hosted on .NET 10 — script projects as an editor asset type, with
live reload of compiled assemblies. Moved out of the engine tree to be built and distributed as a
package.

Published as [`plasma.csharp`](https://plasmaengine.github.io/PlasmaPackages/index.plPackageIndex) in
the Plasma package registry.

## The .NET runtime is optional

The package ships a private .NET 10 runtime as an **optional** component — 77 MB on disk, 36 MB as a
blob. That word is meant literally rather than as a hedge: `plCSharpHost` decides with

```cpp
const bool bUsePrivateRuntime = plOSFile::ExistsFile(sPrivateHostFxr);
hostFxrParameters.dotnet_root = bUsePrivateRuntime ? privateDotNetRootWide.GetData() : nullptr;
```

Install the component and the package is self-contained. Skip it and `hostfxr` resolves a .NET 10
runtime installed on the machine instead — which anyone with the .NET SDK already has.

Blobs are content-addressed, so the runtime is uploaded once and every later version of this package
that does not change it resolves to the same bytes: no re-upload, and no re-download for anyone who
already has it.

## Layout

```
Package.plPackage        the manifest - identity, compatibility, components
CSharp.plPluginBundle    legacy descriptor, kept until the engine reads manifests directly
Source/
  CSharpPlugin/          runtime -> plCSharpPlugin
    Managed/             the .NET projects: ManagedHost, ScriptCore, Engine.Generator, M0Game
  EditorPluginCSharp/    editor  -> plEditorPluginCSharp  (Qt: C# project asset)
  EditorManaged/         Plasma.ScriptInspector, the descriptor tool the editor shells out to
Scripts/                 the managed build and .NET runtime layout scripts
Tests/                   the C# host test, moved here with the code it exercises
Configs/                 build rules for building against an installed engine SDK
Branding/                icon and banner, plus the renderer that produced them
```

## Payload resolution

The plugin's payload — managed assemblies, tools, and the optional runtime — is found relative to
the plugin's own DLL, falling back to the application directory. That fallback is required rather
than defensive: a bundle with `%LoadCopy{true}` is loaded from a copy elsewhere, and that copy has
no payload beside it.

This replaced an unconditional `plOSFile::GetApplicationDirectory()` lookup, which was correct for an
engine build and wrong for a package — an installed package lives in the package store, not next to
the editor executable.

## Dependencies

None resolvable through the registry. The plugin binds to `plCore` and the editor framework, both
part of the engine.

## Building

Requires an engine SDK — a built engine checkout — named by `PLASMA_SDK_ROOT`, plus the .NET 10 SDK
on `PATH` for the managed half:

```bash
PLASMA_SDK_ROOT=/path/to/engine plasmabuild build EditorPluginCSharp
```

The build downloads the nethost pack from NuGet (hash-verified) on first run, builds the managed
projects, and lays out the private runtime under `Bin/Windows_x64_Development/DotNet`. Expect the
first build to take a while and produce ~78 MB of output.

## Publishing a new version

Staging and publishing are driven from the engine and registry repositories:

```bash
# engine repo - stage and validate, emitting machine-readable output
python Utilities/PackageInspector.py <staged-package-dir> --json <out>.json

# registry repo - upload blobs and record the release
python tools/publish_package.py <out>.json
```

Versions are immutable. A mistake is fixed by publishing a new patch version, never by replacing an
existing one — every lock file that already trusts a version must keep resolving to the same bytes.
