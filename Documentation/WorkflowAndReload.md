# C# workflow and reload

Implementation notes, 1 October 2026. Tested on Windows x64 with .NET SDK 10.0.400, Roslyn 5.9 and the package's private .NET 10.0.10 runtime.

## Start a project

Install `plasma.csharp` through the project's package configuration. Use a matching engine/package ABI and a .NET 10 SDK accepted by the project's `global.json`. New generated projects accept newer .NET 10 feature bands. The package uses its bundled runtime first; compatible installed runtime patches can be used when machine resolution is necessary. The engine log identifies the runtime actually selected.

Open the C# project asset. Its **Setup** tab checks packaged runtime/tool files and runs an SDK check in the project's directory. A missing tool means the package should be rebuilt or reinstalled. SDK errors explain the applicable `global.json` policy. ABI validation happens when the hosted runtime initializes; the Setup tab does not simulate that check.

Use **Create Script**, build the project, then assign its compiled script subasset to a Script Component. New scripts have explicit persistent IDs. Keep an existing ID when moving or renaming a script. Give a copied script a new GUID; the generator rejects duplicate identities. Before renaming an older script that has no explicit ID, pin the persistent GUID from its current compiled descriptor into `[PlasmaScript(Id = "...")]`.

**Build Project** queues an asset processor build. Automatic source processing uses the same scheduler. The editor remains available while it compiles. **Problems** lists source diagnostics; activating a row opens the configured IDE at its file and location. **Build Output** retains semantic diagnostics. Stage timings are logged separately so changing milliseconds do not repeatedly modify the asset.

The status reports asset build/import state. “Up to date” confirms the artifact is current; it does not acknowledge the revision currently running in another process. Consult the engine log for generation load/update messages.

## Edit during simulation

Use Development configuration for in-place updates: optimization disabled, debug symbols enabled and portable PDBs. Start simulation, change an eligible existing method body, save and let the asset processor publish it. The editor engine enables modifiable assemblies before CoreCLR starts. The host then checks actual runtime support and applies updates only when no managed callback is executing.

| Edit or condition | Behavior |
| --- | --- |
| Compatible existing method body | Attempt an in-place update; keep objects, fields and cached generated lifecycle delegates. |
| Literal change with the same supported local declarations and syntax shape | Attempt a mapped update; adding/removing locals can fall back. |
| Compile/generator error | Retain the last successful artifact and running code; show diagnostics. |
| Fields, attributes, constructor, property, script identity or generated metadata changes | Full reload. |
| Async/iterator/lambda/local-function method edit | Full reload under the initial conservative policy. |
| Binding schema, helper DLL or project/reference configuration changes | Full reload; shared host/package upgrades can require a process restart. |
| Attached managed debugger | Full reload; Plasma does not compete with the debugger for delta ownership. |
| Unsupported runtime, busy host, expired baseline or rejected delta | Full reload. |
| Unexpected delta application failure | Mark that context unsuitable for reuse and load a full replacement. |

An in-place update does not rerun constructors, field initializers, `Initialize`, activation or simulation-start callbacks. Existing fields keep their current values. A call already executing finishes its existing code; subsequent calls can run the new implementation. See [.NET's update API](https://learn.microsoft.com/en-us/dotnet/api/system.reflection.metadata.metadataupdater.applyupdate?view=net-10.0).

The compiler retains ordered chains for up to eight exact PE/PDB baselines, bounded to 64 deltas and 16 MiB per chain. The runtime records both the applied sequence and hashes of its applied prefix, so a restarted compiler cannot substitute a divergent chain with the same sequence number. It can consume intervening revisions after skipped imports. Expired/rejected chains use a full artifact. This design uses replayable chains rather than an engine-to-compiler acknowledgement RPC. It does not expose a complete live-revision status in the editor yet.

To use the ordinary build path, disable **Use Persistent Compiler** in the project asset. That also removes in-place updates from newly built artifacts. Other configurations use the normal CLI build and their own optimization settings. Restart simulation to establish a fresh baseline when necessary.

## Keep gameplay state across a full reload

Implement `IScriptReloadState` when private state must survive structural edits. Return versioned JSON from `CaptureReloadState`; validate and restore it in `RestoreReloadState`. The payload is limited to one MiB.

Capture must have no side effects. Persist game values and durable logical identities. Reacquire world objects/components after replacement. Do not serialize native handles, delegates, cancellation sources, subscriptions, or background tasks.

Exposed values are applied to the candidate before private state restoration. Restoration runs before `Initialize`; normal activation and simulation-start processing follows the swap. A failed capture, conversion, restore or initialization preserves the previous usable instance. Candidate cleanup is attempted, but user-defined external side effects are not automatically rolled back. Pair subscriptions and other acquired resources with cleanup that tolerates partial initialization.

BrickBreaker stores score, lives, bricks, round state and its accumulator in a versioned JSON record. On a full reload it reacquires scene references and skips the ordinary new-game reset. In-place updates keep those values directly in the existing instance.

Old generations retire through weak references and deferred checks. Resource loading no longer waits through repeated collections/finalizers. Sixteen pending retired contexts stop additional full loads until collection or restart allows progress. An overdue context is not proof of a permanent leak. Background GC can still pause managed execution; memory and long-running retirement behavior need broader stress measurement.

An unhandled Update exception pauses Update for that instance and logs its source/context. A successful code revision, reactivation callback or exposed-parameter change allows another attempt. Other lifecycle and message failures retain their existing reporting behavior.

## Inspector and API contracts

Scalars, strings, enum values, time/angle, supported vectors, quaternion, color and transform retain their existing conversion support. Normal enums fitting signed 32-bit values use named controls. Nonnegative flags expose individual named bits; composite aliases combine those bits. Wide enum values and negative flags keep numeric controls with a warning.

`[FormerlySerializedAs("OldName")]` maps an old exposed parameter name to its current member when native values load or migrate. Multiple unambiguous former names are supported. An explicitly stored current-name value wins over an old alias. Keep aliases while scenes/prefabs still contain those names. This does not rewrite custom JSON/save data, or automatically canonicalize every old property-grid entry.

Range and category metadata are supported. Display-name metadata is generated but universal label/tooltip presentation is still pending. Durable GameObject/Component/resource inspector assignments and collection editing remain follow-up work; do not use transient native handles as serialized references.

Generated component wrappers implement `INativeComponent<T>`. `TryGetComponent<T>` and `CreateComponent<T>` use their native mapping and factory. Custom wrappers must implement that contract. `NativeObject.IsValid` remains a cheap structural test; `IsAlive` asks the native registry whether the object still exists, in the appropriate engine execution scope.

## Background work

Native engine APIs require the appropriate world/thread scope. Do I/O and engine-independent calculation in background work, passing `LifetimeCancellation` to cancellable operations. Full reload/destruction cancels the instance's lifetime; an in-place update retains it.

Use `PostToUpdate(Action)` to apply a completed result synchronously in the next Update world scope. The script must override Update. The queue accepts at most 256 pending actions and drains at most 64 per Update. A false result means dispatch is unavailable, the instance has detached, or the queue is full. Async delegates are rejected because their continuation could escape the native scope. Destruction clears pending actions. Cancellation is cooperative; scripts must release subscriptions and tasks they create.

## Debugging

Open the project in the configured IDE, start simulation, and attach a managed debugger to the actual `EditorEngineProcess.exe` child, rather than the editor UI or compiler server. For standalone play attach to this project's `Player.exe`. Use the process's current PID after a restart. Portable PDBs are packaged beside gameplay assemblies; preserve source paths when moving a project.

VS Code supports C# process attachment through its C# extension; see [its debugger documentation](https://code.visualstudio.com/docs/csharp/debugging). Visual Studio provides [Attach to Process](https://learn.microsoft.com/en-us/visualstudio/debugger/attach-to-running-processes-with-the-visual-studio-debugger?view=visualstudio). Debugger attachment, active statements and moved-source mapping have not been validated as a release matrix in this implementation. An editor action that prepares the current PID is still pending.

## Compiler compatibility and deployment

The compiler service retains evaluated Compile items, references, syntax trees, generators and analyzer state. It checks relevant project/import/reference/source inputs and verifies they remain coherent before publishing. Service identity includes tool dependency contents; installed tools can update while an older service runs its isolated copy. Idle project services exit after 15 minutes. Immutable tool cache folders are not yet pruned automatically.

Custom targets/imports, project references, signing, native resources, custom analyzer configuration and non-Development builds use the CLI fallback. Arbitrary MSBuild behavior is not emulated by the retained compiler. Use the standard compiler for unsupported project features. Full build-system parity and automatic watching of every external linked source remain validation/implementation work.

`PlasmaSeparateBindings=true` builds cached engine bindings into `Plasma.ProjectBindings.dll` in the service path. CLI fallback emits normal bindings into the gameplay assembly. User partial extensions of generated binding types cannot span the separate assembly; remove the opt-in for such projects.

BrickBreaker's shared properties resolve its C# package version from `Editor/Packages.ddl`. `Build.ps1` stages engine/package/runtime files into `Intermediate/Runtime/Binaries`, preserving the SDK's Player payload. Close that project's running Player before rebuilding its deployment. Shipping optimizes gameplay through the standard compiler; a Development Player smoke test is distinct from a full Shipping export test.

## Reproduce validation

From the package directory:

```powershell
dotnet test Source/CSharpPlugin/Managed/Plasma.ManagedTests/Plasma.ManagedTests.csproj -c Development
plasmabuild build Source/EditorPluginCSharp/EditorPluginCSharp.target.cs -c Development -j 4
plasmabuild build Source/CSharpRuntimeTests/CSharpRuntimeTests.target.cs -c Development -j 4
./Scripts/test-csharp-reload.ps1 -SampleRoot E:/PlasmaTech/BrickBreakerSamples/BrickBreaker-CSharp -SdkRoot E:/PlasmaTech/PlasmaEngine2
```

From BrickBreaker run `./Build.ps1 -Test` with the appropriate SDK/package paths. Run `Tests/Mechanics.plScene` in Player to execute the checks through the actual hosted engine.

The reload suite copies sample sources into an isolated fixture, exercises cached delegates, mapped locals, private-state preservation, syntax-error recovery, skipped imports, structural fallback, 100 further edits, changed helpers and the real native script adapter. It saves raw timing CSVs. These timings cover compiler and isolated host work; they exclude editor watching, queueing, import/IPC and the first updated rendered frame. Full save-to-execution targets remain unverified.
