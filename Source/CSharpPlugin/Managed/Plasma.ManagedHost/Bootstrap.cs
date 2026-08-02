using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;

namespace Plasma.ManagedHost;

public static unsafe class Bootstrap
{
    private const uint AbiVersion = 1;
    private const uint ValueLayoutVersion = 2;
    private const ulong BufferOwnerToken = 0x504C435342554646UL;
    private const int MaxUnloadGcCycles = 12;
    private static readonly uint NativeApiV1BaseSize =
        checked((uint)Marshal.OffsetOf<NativeApiV1>(nameof(NativeApiV1.M0Probe)));
    private static readonly uint ManagedApiV1BaseSize =
        checked((uint)Marshal.OffsetOf<ManagedApiV1>(nameof(ManagedApiV1.RunM0Probe)));

    private static readonly object Sync = new();
    private static readonly Dictionary<ulong, Generation> Generations = [];
    private static readonly Dictionary<ulong, Instance> Instances = [];

    private static NativeApiV1 s_nativeApi;
    private static delegate* unmanaged[Cdecl]<long, long*, CSharpStatus> s_nativeM0Probe;
    private static bool s_initialized;
    private static long s_nextGeneration;
    private static long s_nextInstance;

    [ThreadStatic]
    private static string? s_lastError;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static CSharpStatus Initialize(NativeApiV1* nativeApi, ManagedApiV1* managedApi)
    {
        try
        {
            if (nativeApi is null || managedApi is null)
            {
                return Fail(CSharpStatus.InvalidArgument, "Native and managed API pointers must both be non-null.");
            }

            uint nativeApiSize = nativeApi->Size;
            uint managedApiCapacity = managedApi->Size;
            if (nativeApiSize < NativeApiV1BaseSize ||
                managedApiCapacity < ManagedApiV1BaseSize)
            {
                return Fail(CSharpStatus.AbiMismatch,
                    $"C# ABI table is too small. Native/managed capacities were " +
                    $"{nativeApiSize}/{managedApiCapacity}; required base sizes are " +
                    $"{NativeApiV1BaseSize}/{ManagedApiV1BaseSize}.");
            }

            if (nativeApi->Version != AbiVersion ||
                nativeApi->PointerSize != (uint)sizeof(void*) ||
                nativeApi->ValueLayoutVersion != ValueLayoutVersion)
            {
                return Fail(CSharpStatus.AbiMismatch,
                    $"C# ABI mismatch. Native size/version/pointer/value-layout were " +
                    $"{nativeApiSize}/{nativeApi->Version}/{nativeApi->PointerSize}/{nativeApi->ValueLayoutVersion}; " +
                    $"managed requires native/managed base sizes {NativeApiV1BaseSize}/{ManagedApiV1BaseSize}, " +
                    $"version {AbiVersion}, pointer size {sizeof(void*)}, and value-layout {ValueLayoutVersion}.");
            }

            string? layoutError = ValidateValueLayouts();
            if (layoutError is not null)
            {
                return Fail(CSharpStatus.AbiMismatch, layoutError);
            }

            lock (Sync)
            {
                s_nativeApi = new NativeApiV1
                {
                    Size = nativeApiSize,
                    Version = nativeApi->Version,
                    PointerSize = nativeApi->PointerSize,
                    ValueLayoutVersion = nativeApi->ValueLayoutVersion,
                    Log = nativeApi->Log,
                    ValidateObject = nativeApi->ValidateObject,
                    InvokeReflected = nativeApi->InvokeReflected,
                    ReleaseNativeValue = nativeApi->ReleaseNativeValue,
                    QueryExtension = nativeApi->QueryExtension,
                };
                s_nativeM0Probe = nativeApiSize >= (uint)sizeof(NativeApiV1)
                    ? nativeApi->M0Probe
                    : null;
                NativeBridge.Configure((nint)nativeApi);
                s_initialized = true;
            }

            managedApi->Size = managedApiCapacity >= (uint)sizeof(ManagedApiV1)
                ? (uint)sizeof(ManagedApiV1)
                : ManagedApiV1BaseSize;
            managedApi->Version = AbiVersion;
            managedApi->PointerSize = (uint)sizeof(void*);
            managedApi->ValueLayoutVersion = ValueLayoutVersion;
            managedApi->Shutdown = &ShutdownExport;
            managedApi->LoadGeneration = &LoadGenerationExport;
            managedApi->GetGenerationDescriptor = &GetGenerationDescriptorExport;
            managedApi->CreateInstance = &CreateInstanceExport;
            managedApi->DestroyInstance = &DestroyInstanceExport;
            managedApi->InvokeMethod = &InvokeMethodExport;
            managedApi->GetField = &GetFieldExport;
            managedApi->SetField = &SetFieldExport;
            managedApi->DispatchMessage = &DispatchMessageExport;
            managedApi->UnloadGeneration = &UnloadGenerationExport;
            managedApi->GetLastError = &GetLastErrorExport;
            managedApi->FreeBuffer = &FreeBufferExport;
            managedApi->ReleaseValue = &ReleaseValueExport;
            managedApi->QueryExtension = &QueryExtensionExport;
            if (managedApiCapacity >= (uint)sizeof(ManagedApiV1))
            {
                managedApi->RunM0Probe = &RunM0ProbeExport;
            }

            return CSharpStatus.Success;
        }
        catch (Exception exception)
        {
            return Fail(CSharpStatus.ManagedException, exception);
        }
    }

    private static string? ValidateValueLayouts()
    {
        (string Name, int Actual, int Expected)[] layouts =
        [
            (nameof(CSharpObjectHandle), sizeof(CSharpObjectHandle), 24),
            (nameof(CSharpValue), sizeof(CSharpValue), 24),
            (nameof(Time), Unsafe.SizeOf<Time>(), 8),
            (nameof(Angle), Unsafe.SizeOf<Angle>(), 4),
            (nameof(Vec2), Unsafe.SizeOf<Vec2>(), 8),
            (nameof(Vec2U32), Unsafe.SizeOf<Vec2U32>(), 8),
            (nameof(Vec3), Unsafe.SizeOf<Vec3>(), 12),
            (nameof(Vec4), Unsafe.SizeOf<Vec4>(), 16),
            (nameof(Quat), Unsafe.SizeOf<Quat>(), 16),
            (nameof(Color), Unsafe.SizeOf<Color>(), 16),
            (nameof(Transform), Unsafe.SizeOf<Transform>(), 40),
        ];

        foreach ((string name, int actual, int expected) in layouts)
        {
            if (actual != expected)
            {
                return $"C# value layout mismatch for {name}: managed size is {actual}, native contract requires {expected}.";
            }
        }

        return null;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus ShutdownExport()
    {
        try
        {
            ulong[] generationIds = DetachAllInstancesForShutdown();

            CSharpStatus result = CSharpStatus.Success;
            foreach (ulong generationId in generationIds)
            {
                CSharpStatus unloadStatus = UnloadGenerationCore(generationId, out _);
                if (unloadStatus != CSharpStatus.Success)
                {
                    result = unloadStatus;
                }
            }

            lock (Sync)
            {
                s_initialized = false;
                NativeBridge.Reset();
                s_nativeApi = default;
                s_nativeM0Probe = null;
            }

            return result;
        }
        catch (Exception exception)
        {
            return Fail(CSharpStatus.ManagedException, exception);
        }
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static ulong[] DetachAllInstancesForShutdown()
    {
        List<Instance> instances;
        ulong[] generationIds;
        lock (Sync)
        {
            instances = [.. Instances.Values];
            Instances.Clear();
            generationIds = [.. Generations.Keys];
        }

        foreach (Instance instance in instances)
        {
            instance.Script.__Detach();
        }

        return generationIds;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus LoadGenerationExport(CSharpGenerationLoadDesc* desc, ulong* generation)
    {
        try
        {
            if (!s_initialized)
            {
                return Fail(CSharpStatus.NotInitialized, "The managed bootstrap has not been initialized.");
            }

            if (desc is null || generation is null ||
                desc->Size < (uint)sizeof(CSharpGenerationLoadDesc) || desc->Version != 1)
            {
                return Fail(CSharpStatus.InvalidArgument, "Invalid generation load descriptor.");
            }

            string assemblyPath = Decode(desc->AssemblyPath);
            if (!Path.IsPathFullyQualified(assemblyPath) || !File.Exists(assemblyPath))
            {
                return Fail(CSharpStatus.InvalidArgument,
                    $"Managed generation assembly '{assemblyPath}' does not exist or is not an absolute path.");
            }

            ulong id = checked((ulong)Interlocked.Increment(ref s_nextGeneration));
            string requestedShadowRoot = Decode(desc->ShadowCopyRoot);
            string shadowRoot = string.IsNullOrWhiteSpace(requestedShadowRoot)
                ? Path.Combine(Path.GetTempPath(), "Plasma", "CSharp", "Generations")
                : Path.GetFullPath(requestedShadowRoot);
            string shadowDirectory = Path.Combine(shadowRoot, id.ToString("X16", CultureInfo.InvariantCulture));
            ValidateShadowCopyLocation(assemblyPath, shadowDirectory);

            GenerationLoadContext? loadContext = null;
            try
            {
                string shadowAssemblyPath = ShadowCopyAssemblyDirectory(assemblyPath, shadowDirectory);
                loadContext = new GenerationLoadContext(shadowAssemblyPath);
                Assembly assembly = loadContext.LoadFromAssemblyPath(shadowAssemblyPath);
                IReadOnlyList<ScriptTypeDescriptor> descriptors = DiscoverDescriptors(assembly);
                byte[] manifest = BuildManifest(id, assembly, descriptors);
                var loadedGeneration = new Generation(id, loadContext, shadowDirectory, descriptors, manifest);

                lock (Sync)
                {
                    Generations.Add(id, loadedGeneration);
                }

                *generation = id;
                return CSharpStatus.Success;
            }
            catch
            {
                loadContext?.Unload();
                TryDeleteDirectory(shadowDirectory);
                throw;
            }
        }
        catch (Exception exception)
        {
            return Fail(CSharpStatus.AssemblyLoadFailed, exception);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus GetGenerationDescriptorExport(ulong generationId, CSharpBuffer* descriptor)
    {
        try
        {
            if (descriptor is null)
            {
                return Fail(CSharpStatus.InvalidArgument, "The descriptor output buffer is null.");
            }

            Generation generation;
            lock (Sync)
            {
                if (!Generations.TryGetValue(generationId, out generation!))
                {
                    return Fail(CSharpStatus.GenerationNotFound, $"C# generation {generationId} was not found.");
                }
            }

            return AllocateBuffer(generation.Manifest, descriptor);
        }
        catch (Exception exception)
        {
            return Fail(CSharpStatus.ManagedException, exception);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus CreateInstanceExport(CSharpInstanceCreateDesc* desc, ulong* instanceId)
    {
        try
        {
            if (desc is null || instanceId is null ||
                desc->Size < (uint)sizeof(CSharpInstanceCreateDesc) || desc->Version != 1)
            {
                return Fail(CSharpStatus.InvalidArgument, "Invalid C# instance creation descriptor.");
            }

            Generation generation;
            ScriptTypeDescriptor type;
            lock (Sync)
            {
                if (!Generations.TryGetValue(desc->Generation, out generation!))
                {
                    return Fail(CSharpStatus.GenerationNotFound, $"C# generation {desc->Generation} was not found.");
                }

                if (!generation.Types.TryGetValue(desc->TypeId, out type!))
                {
                    return Fail(CSharpStatus.TypeNotFound,
                        $"C# type 0x{desc->TypeId:X16} was not found in generation {desc->Generation}.");
                }
            }

            ComponentScript script = type.Create();
            var ownerContext = new ScriptOwnerContext(
                desc->Owner.ToNativeObject(),
                desc->World.ToNativeObject(),
                desc->OwnerComponent.ToNativeObject());
            script.__Attach(in ownerContext);

            ulong id = checked((ulong)Interlocked.Increment(ref s_nextInstance));
            lock (Sync)
            {
                if (!Generations.ContainsKey(desc->Generation))
                {
                    script.__Detach();
                    return Fail(CSharpStatus.GenerationNotFound,
                        $"C# generation {desc->Generation} was unloaded while creating an instance.");
                }

                Instances.Add(id, new Instance(id, desc->Generation, type, script));
            }

            *instanceId = id;
            return CSharpStatus.Success;
        }
        catch (Exception exception)
        {
            return Fail(CSharpStatus.ManagedException, exception);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus DestroyInstanceExport(ulong instanceId)
    {
        try
        {
            Instance instance;
            lock (Sync)
            {
                if (!Instances.Remove(instanceId, out instance!))
                {
                    return Fail(CSharpStatus.InstanceNotFound, $"C# instance {instanceId} was not found.");
                }
            }

            instance.Script.__Detach();
            return CSharpStatus.Success;
        }
        catch (Exception exception)
        {
            return Fail(CSharpStatus.ManagedException, exception);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus InvokeMethodExport(
        ulong instanceId,
        ulong methodId,
        CSharpValue* arguments,
        uint argumentCount,
        CSharpValue* result)
    {
        Instance? diagnosticInstance = null;
        string lifecycleName = methodId <= (ulong)ScriptLifecycleMethod.Update
            ? ((ScriptLifecycleMethod)methodId).ToString()
            : $"unknown lifecycle method {methodId}";
        try
        {
            if (result is null || (argumentCount > 0 && arguments is null))
            {
                return Fail(CSharpStatus.InvalidArgument, "Invalid C# method invocation arguments.");
            }

            if (!TryGetInstance(instanceId, out Instance instance, out CSharpStatus status))
            {
                return status;
            }
            diagnosticInstance = instance;

            if (methodId > (ulong)ScriptLifecycleMethod.Update ||
                !instance.Type.Lifecycle.TryGetValue((ScriptLifecycleMethod)methodId, out ScriptLifecycleInvoker? invoke))
            {
                return Fail(CSharpStatus.MemberNotFound,
                    $"C# script '{instance.Type.ManagedName}' does not implement '{lifecycleName}'. " +
                    "Rebuild the C# scripts if this method should be present.");
            }

            Time deltaTime = Time.Zero;
            if ((ScriptLifecycleMethod)methodId == ScriptLifecycleMethod.Update)
            {
                if (argumentCount != 1)
                {
                    return Fail(CSharpStatus.InvalidArgument, "C# Update requires one time argument.");
                }

                object? converted = ConvertFromValue(*arguments, typeof(double));
                deltaTime = Time.FromSeconds((double)converted!);
            }
            else if (argumentCount != 0)
            {
                return Fail(CSharpStatus.InvalidArgument, "This C# lifecycle method takes no arguments.");
            }

            using ScriptExecutionContext.Scope scope =
                ScriptExecutionContext.Enter(instance.Script.World);
            invoke(instance.Script, deltaTime);
            *result = default;
            return CSharpStatus.Success;
        }
        catch (InvalidValueException exception)
        {
            return Fail(
                CSharpStatus.InvalidValue,
                FormatScriptException(
                    $"C# script '{diagnosticInstance?.Type.ManagedName ?? "<unknown>"}': {lifecycleName}",
                    instanceId,
                    diagnosticInstance,
                    exception));
        }
        catch (Exception exception)
        {
            return Fail(
                CSharpStatus.ManagedException,
                FormatScriptException(
                    $"C# script '{diagnosticInstance?.Type.ManagedName ?? "<unknown>"}': {lifecycleName}",
                    instanceId,
                    diagnosticInstance,
                    exception));
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus GetFieldExport(ulong instanceId, ulong fieldId, CSharpValue* value)
    {
        Instance? diagnosticInstance = null;
        try
        {
            if (value is null)
            {
                return Fail(CSharpStatus.InvalidArgument, "The C# field output value is null.");
            }

            if (!TryGetInstance(instanceId, out Instance instance, out CSharpStatus status))
            {
                return status;
            }
            diagnosticInstance = instance;

            if (!instance.Type.Fields.TryGetValue(fieldId, out ScriptFieldDescriptor? field))
            {
                return Fail(CSharpStatus.MemberNotFound,
                    $"Field 0x{fieldId:X16} was not found on C# type '{instance.Type.ManagedName}'.");
            }

            return ConvertToValue(field.Get(instance.Script), value);
        }
        catch (InvalidValueException exception)
        {
            return Fail(
                CSharpStatus.InvalidValue,
                FormatScriptException(
                    $"Get field 0x{fieldId:X16}",
                    instanceId,
                    diagnosticInstance,
                    exception));
        }
        catch (Exception exception)
        {
            return Fail(
                CSharpStatus.ManagedException,
                FormatScriptException(
                    $"Get field 0x{fieldId:X16}",
                    instanceId,
                    diagnosticInstance,
                    exception));
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus SetFieldExport(ulong instanceId, ulong fieldId, CSharpValue* value)
    {
        Instance? diagnosticInstance = null;
        try
        {
            if (value is null)
            {
                return Fail(CSharpStatus.InvalidArgument, "The C# field input value is null.");
            }

            if (!TryGetInstance(instanceId, out Instance instance, out CSharpStatus status))
            {
                return status;
            }
            diagnosticInstance = instance;

            if (!instance.Type.Fields.TryGetValue(fieldId, out ScriptFieldDescriptor? field))
            {
                return Fail(CSharpStatus.MemberNotFound,
                    $"Field 0x{fieldId:X16} was not found on C# type '{instance.Type.ManagedName}'.");
            }

            field.Set(instance.Script, ConvertFromValue(*value, field.ValueType));
            return CSharpStatus.Success;
        }
        catch (InvalidValueException exception)
        {
            return Fail(
                CSharpStatus.InvalidValue,
                FormatScriptException(
                    $"Set field 0x{fieldId:X16}",
                    instanceId,
                    diagnosticInstance,
                    exception));
        }
        catch (Exception exception)
        {
            return Fail(
                CSharpStatus.ManagedException,
                FormatScriptException(
                    $"Set field 0x{fieldId:X16}",
                    instanceId,
                    diagnosticInstance,
                    exception));
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus DispatchMessageExport(ulong instanceId, ulong messageId, CSharpValue* payload)
    {
        Instance? diagnosticInstance = null;
        try
        {
            if (payload is null)
            {
                return Fail(CSharpStatus.InvalidArgument, "The C# message payload is null.");
            }

            if (!TryGetInstance(instanceId, out Instance instance, out CSharpStatus status))
            {
                return status;
            }
            diagnosticInstance = instance;

            if (!instance.Type.Messages.TryGetValue(messageId, out ScriptMessageDescriptor? message))
            {
                return Fail(CSharpStatus.MemberNotFound,
                    $"Message handler 0x{messageId:X16} was not found on C# type '{instance.Type.ManagedName}'.");
            }

            object? managedPayload = ConvertFromValue(*payload, message.ValueType);
            object?[] values = [managedPayload];
            using ScriptExecutionContext.Scope scope =
                ScriptExecutionContext.Enter(instance.Script.World);
            message.Invoke(instance.Script, values);
            return CSharpStatus.Success;
        }
        catch (InvalidValueException exception)
        {
            return Fail(
                CSharpStatus.InvalidValue,
                FormatScriptException(
                    $"Dispatch message 0x{messageId:X16}",
                    instanceId,
                    diagnosticInstance,
                    exception));
        }
        catch (Exception exception)
        {
            return Fail(
                CSharpStatus.ManagedException,
                FormatScriptException(
                    $"Dispatch message 0x{messageId:X16}",
                    instanceId,
                    diagnosticInstance,
                    exception));
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus UnloadGenerationExport(ulong generationId, CSharpUnloadReport* report)
    {
        try
        {
            CSharpStatus status = UnloadGenerationCore(generationId, out CSharpUnloadReport unloadReport);
            if (report is not null)
            {
                *report = unloadReport;
            }

            return status;
        }
        catch (Exception exception)
        {
            return Fail(CSharpStatus.ManagedException, exception);
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus GetLastErrorExport(CSharpBuffer* error)
    {
        try
        {
            if (error is null)
            {
                return CSharpStatus.InvalidArgument;
            }

            return AllocateBuffer(Encoding.UTF8.GetBytes(s_lastError ?? string.Empty), error);
        }
        catch
        {
            return CSharpStatus.ManagedException;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus FreeBufferExport(CSharpBuffer* buffer)
    {
        if (buffer is null)
        {
            return CSharpStatus.InvalidArgument;
        }

        if (buffer->Data is not null && buffer->OwnerToken != BufferOwnerToken)
        {
            return Fail(CSharpStatus.InvalidArgument, "Attempted to free a buffer not owned by the managed bootstrap.");
        }

        NativeMemory.Free(buffer->Data);
        *buffer = default;
        return CSharpStatus.Success;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus ReleaseValueExport(CSharpValue* value)
    {
        if (value is null)
        {
            return CSharpStatus.InvalidArgument;
        }

        if ((value->Flags & CSharpValueFlags.ManagedOwned) != 0)
        {
            NativeMemory.Free((void*)value->Payload0);
        }

        *value = default;
        return CSharpStatus.Success;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus QueryExtensionExport(CSharpUtf8Span name, uint minimumVersion, void** api)
    {
        if (api is not null)
        {
            *api = null;
        }

        return Fail(CSharpStatus.Unsupported,
            $"Managed C# extension '{Decode(name)}' version {minimumVersion} is not available.");
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static CSharpStatus RunM0ProbeExport(long input, long* output)
    {
        if (output is null)
        {
            return Fail(CSharpStatus.InvalidArgument, "The M0 probe output pointer is null.");
        }

        if (!s_initialized || s_nativeM0Probe == null)
        {
            return Fail(CSharpStatus.NotInitialized, "The native M0 probe callback is unavailable.");
        }

        return s_nativeM0Probe(input, output);
    }

    private static CSharpStatus UnloadGenerationCore(ulong generationId, out CSharpUnloadReport report)
    {
        report = new CSharpUnloadReport
        {
            Size = (uint)sizeof(CSharpUnloadReport),
            Version = 1,
        };

        UnloadTicket? ticket = BeginUnload(generationId, out uint liveInstances, out CSharpStatus status);
        report.LiveInstances = liveInstances;
        if (ticket is null)
        {
            return status;
        }

        uint gcCycles = 0;
        while (ticket.ContextReference.IsAlive && gcCycles < MaxUnloadGcCycles)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            ++gcCycles;
        }

        report.GcCycles = gcCycles;
        report.LoadContextAlive = ticket.ContextReference.IsAlive ? 1U : 0U;
        if (report.LoadContextAlive != 0)
        {
            return Fail(CSharpStatus.UnloadIncomplete,
                $"C# generation {generationId} remained alive after {gcCycles} GC cycles.");
        }

        TryDeleteDirectory(ticket.ShadowDirectory);
        return CSharpStatus.Success;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static UnloadTicket? BeginUnload(
        ulong generationId,
        out uint liveInstances,
        out CSharpStatus status)
    {
        Generation generation;
        lock (Sync)
        {
            if (!Generations.TryGetValue(generationId, out generation!))
            {
                liveInstances = 0;
                status = Fail(CSharpStatus.GenerationNotFound, $"C# generation {generationId} was not found.");
                return null;
            }

            liveInstances = checked((uint)Instances.Values.Count(instance => instance.GenerationId == generationId));
            if (liveInstances != 0)
            {
                status = Fail(CSharpStatus.GenerationInUse,
                    $"C# generation {generationId} still owns {liveInstances} live instance(s).");
                return null;
            }

            Generations.Remove(generationId);
        }

        WeakReference contextReference = new(generation.LoadContext, trackResurrection: false);
        string shadowDirectory = generation.ShadowDirectory;
        generation.ReleaseAndUnload();
        status = CSharpStatus.Success;
        return new UnloadTicket(contextReference, shadowDirectory);
    }

    private static string FormatScriptException(
        string operation,
        ulong requestedInstanceId,
        Instance? instance,
        Exception exception)
    {
        string sourceLocation = FindUserSourceLocation(exception) is { } location
            ? $" Source: {location}."
            : string.Empty;

        if (instance is null)
        {
            return $"{operation} failed for instance 0x{requestedInstanceId:X16}; " +
                   $"the instance context was unavailable.{sourceLocation}" +
                   $"{Environment.NewLine}{exception}";
        }

        ComponentScript script = instance.Script;
        return $"{operation} failed.{sourceLocation} " +
               $"Generation=0x{instance.GenerationId:X16}; " +
               $"ManagedType='{instance.Type.ManagedName}'; " +
               $"Instance=0x{instance.Id:X16}; " +
               $"Owner={FormatObjectHandle(script.Owner.Native)}; " +
               $"World={FormatObjectHandle(script.World.Native)}; " +
               $"OwnerComponent={FormatObjectHandle(script.OwnerComponent.Native)}." +
               $"{Environment.NewLine}{exception}";
    }

    private static string? FindUserSourceLocation(Exception exception)
    {
        string? fallbackLocation = null;
        for (Exception? current = exception; current is not null; current = current.InnerException)
        {
            if (fallbackLocation is null &&
                current.Data["Plasma.ScriptSourceFile"] is string sourceFile &&
                current.Data["Plasma.ScriptSourceLine"] is int sourceLine &&
                sourceLine > 0)
            {
                fallbackLocation = current.Data["Plasma.ScriptSourceColumn"] is int sourceColumn &&
                                   sourceColumn > 0
                    ? $"{sourceFile}({sourceLine},{sourceColumn})"
                    : $"{sourceFile}({sourceLine})";
            }

            var stackTrace = new StackTrace(current, fNeedFileInfo: true);
            foreach (StackFrame frame in stackTrace.GetFrames())
            {
                string? file = frame.GetFileName();
                int line = frame.GetFileLineNumber();
                if (string.IsNullOrWhiteSpace(file) || line <= 0)
                {
                    continue;
                }

                string portablePath = file.Replace('\\', '/');
                if (portablePath.Contains("/obj/", StringComparison.OrdinalIgnoreCase) ||
                    portablePath.Contains("/bin/", StringComparison.OrdinalIgnoreCase) ||
                    portablePath.Contains("/Plasma.ScriptCore/", StringComparison.OrdinalIgnoreCase) ||
                    portablePath.Contains("/Plasma.ManagedHost/", StringComparison.OrdinalIgnoreCase) ||
                    portablePath.Contains("/Plasma.Engine.Generator/", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                int column = frame.GetFileColumnNumber();
                return column > 0
                    ? $"{file}({line},{column})"
                    : $"{file}({line})";
            }
        }

        return fallbackLocation;
    }

    private static string FormatObjectHandle(NativeObject value) =>
        $"{value.Kind}:0x{value.Id:X16}@0x{value.Generation:X16}";

    private static bool TryGetInstance(
        ulong instanceId,
        out Instance instance,
        out CSharpStatus status)
    {
        lock (Sync)
        {
            if (Instances.TryGetValue(instanceId, out Instance? found))
            {
                instance = found;
                status = CSharpStatus.Success;
                return true;
            }
        }

        instance = null!;
        status = Fail(CSharpStatus.InstanceNotFound, $"C# instance {instanceId} was not found.");
        return false;
    }

    private static IReadOnlyList<ScriptTypeDescriptor> DiscoverDescriptors(Assembly assembly)
    {
        var descriptors = new SortedDictionary<ulong, ScriptTypeDescriptor>();
        foreach (Type providerType in assembly.GetTypes()
                     .Where(type => !type.IsAbstract && typeof(IScriptDescriptorProvider).IsAssignableFrom(type))
                     .OrderBy(type => type.FullName, StringComparer.Ordinal))
        {
            if (Activator.CreateInstance(providerType) is not IScriptDescriptorProvider provider)
            {
                continue;
            }

            foreach (ScriptTypeDescriptor descriptor in provider.GetDescriptors()
                         .OrderBy(descriptor => descriptor.Id))
            {
                if (!descriptors.TryAdd(descriptor.Id, descriptor))
                {
                    throw new InvalidDataException(
                        $"Duplicate C# script type ID 0x{descriptor.Id:X16} in '{assembly.FullName}'.");
                }
            }
        }

        if (descriptors.Count == 0)
        {
            throw new InvalidDataException(
                $"Assembly '{assembly.FullName}' did not expose an {nameof(IScriptDescriptorProvider)}.");
        }

        return [.. descriptors.Values];
    }

    private static byte[] BuildManifest(
        ulong generationId,
        Assembly assembly,
        IReadOnlyList<ScriptTypeDescriptor> descriptors)
    {
        var manifest = new
        {
            abiVersion = AbiVersion,
            valueLayoutVersion = ValueLayoutVersion,
            generation = HexId(generationId),
            assembly = assembly.GetName().Name,
            types = descriptors.Select(type => new
            {
                id = HexId(type.Id),
                persistentId = type.PersistentId,
                managedName = type.ManagedName,
                lifecycle = type.Lifecycle.Keys
                    .OrderBy(method => method)
                    .Select(method => new { id = HexId((ulong)method), name = method.ToString() }),
                fields = type.Fields.Values
                    .OrderBy(field => field.Id)
                    .Select(field => new
                    {
                        id = HexId(field.Id),
                        field.Name,
                        managedType = field.ValueType.FullName,
                        defaultValue = field.DefaultValue?.ToString(),
                        editorMetadata = field.EditorMetadata
                            .OrderBy(pair => pair.Key, StringComparer.Ordinal)
                            .ToDictionary(pair => pair.Key, pair => pair.Value),
                    }),
                messages = type.Messages.Values
                    .OrderBy(message => message.Id)
                    .Select(message => new
                    {
                        id = HexId(message.Id),
                        nativeTypeName = message.NativeTypeName,
                    }),
            }),
        };

        return JsonSerializer.SerializeToUtf8Bytes(manifest, new JsonSerializerOptions
        {
            WriteIndented = false,
            PropertyNamingPolicy = null,
        });
    }

    private static string HexId(ulong value) =>
        value.ToString("X16", CultureInfo.InvariantCulture);

    private static string ShadowCopyAssemblyDirectory(string assemblyPath, string shadowDirectory)
    {
        ValidateShadowCopyLocation(assemblyPath, shadowDirectory);

        string sourceDirectory = Path.GetFullPath(Path.GetDirectoryName(assemblyPath)
            ?? throw new InvalidDataException($"Assembly path '{assemblyPath}' has no parent directory."));
        string destinationDirectory = Path.GetFullPath(shadowDirectory);
        Directory.CreateDirectory(destinationDirectory);
        foreach (string sourcePath in Directory.EnumerateFiles(sourceDirectory, "*", SearchOption.AllDirectories))
        {
            string relativePath = Path.GetRelativePath(sourceDirectory, sourcePath);
            string targetPath = Path.Combine(destinationDirectory, relativePath);
            Directory.CreateDirectory(Path.GetDirectoryName(targetPath)!);
            File.Copy(sourcePath, targetPath, overwrite: true);
        }

        return Path.Combine(destinationDirectory, Path.GetFileName(assemblyPath));
    }

    private static void ValidateShadowCopyLocation(string assemblyPath, string shadowDirectory)
    {
        string sourceDirectory = Path.GetFullPath(Path.GetDirectoryName(assemblyPath)
            ?? throw new InvalidDataException($"Assembly path '{assemblyPath}' has no parent directory."));
        string destinationDirectory = Path.GetFullPath(shadowDirectory);
        string sourcePrefix = Path.TrimEndingDirectorySeparator(sourceDirectory) +
                              Path.DirectorySeparatorChar;
        string destinationPrefix = Path.TrimEndingDirectorySeparator(destinationDirectory) +
                                   Path.DirectorySeparatorChar;
        if (destinationPrefix.StartsWith(sourcePrefix, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                $"Shadow-copy directory '{destinationDirectory}' must not be inside assembly directory '{sourceDirectory}'.");
        }
    }

    private static CSharpStatus AllocateBuffer(ReadOnlySpan<byte> bytes, CSharpBuffer* buffer)
    {
        if (buffer is null)
        {
            return CSharpStatus.InvalidArgument;
        }

        void* allocation = NativeMemory.Alloc((nuint)bytes.Length + 1);
        if (allocation is null)
        {
            throw new OutOfMemoryException();
        }

        bytes.CopyTo(new Span<byte>(allocation, bytes.Length));
        ((byte*)allocation)[bytes.Length] = 0;
        *buffer = new CSharpBuffer
        {
            Data = allocation,
            Size = checked((uint)bytes.Length),
            Capacity = checked((uint)bytes.Length + 1),
            OwnerToken = BufferOwnerToken,
        };
        return CSharpStatus.Success;
    }

    private static object? ConvertFromValue(CSharpValue value, Type targetType)
    {
        if (value.Kind == CSharpValueKind.Null)
        {
            return targetType.IsValueType && Nullable.GetUnderlyingType(targetType) is null
                ? throw new InvalidValueException($"Null cannot be assigned to '{targetType}'.")
                : null;
        }

        Type effectiveType = Nullable.GetUnderlyingType(targetType) ?? targetType;
        if (effectiveType.IsDefined(typeof(PlasmaMessageAttribute), inherit: false))
        {
            if (value.Kind != CSharpValueKind.UInt64)
            {
                throw new InvalidValueException(
                    $"Custom C# message '{targetType}' requires a managed registry token, " +
                    $"not C# ABI value kind '{value.Kind}'.");
            }

            return ManagedMessageRegistry.Resolve(value.Payload0, effectiveType);
        }

        if (effectiveType.IsDefined(typeof(GeneratedMessageAttribute), inherit: false))
        {
            return DeserializeGeneratedMessage(value, effectiveType);
        }

        if (value.Kind == CSharpValueKind.ObjectHandle)
        {
            return ConvertObjectHandle(value, effectiveType);
        }

        if (value.Kind == CSharpValueKind.ByteSpan)
        {
            return ConvertBlittableValue(value, effectiveType);
        }

        object raw = value.Kind switch
        {
            CSharpValueKind.Boolean => value.Payload0 != 0,
            CSharpValueKind.Int64 => unchecked((long)value.Payload0),
            CSharpValueKind.UInt64 => value.Payload0,
            CSharpValueKind.Double => BitConverter.Int64BitsToDouble(unchecked((long)value.Payload0)),
            CSharpValueKind.Utf8String => Decode((byte*)value.Payload0, checked((uint)value.Payload1)),
            _ => throw new InvalidValueException($"C# value kind '{value.Kind}' is not supported for managed input."),
        };

        if (effectiveType.IsInstanceOfType(raw))
        {
            return raw;
        }

        if (effectiveType.IsEnum)
        {
            object underlying = Convert.ChangeType(raw, Enum.GetUnderlyingType(effectiveType), CultureInfo.InvariantCulture);
            return Enum.ToObject(effectiveType, underlying);
        }

        try
        {
            return Convert.ChangeType(raw, effectiveType, CultureInfo.InvariantCulture);
        }
        catch (Exception exception) when (exception is InvalidCastException or FormatException or OverflowException)
        {
            throw new InvalidValueException(
                $"Cannot convert C# ABI value kind '{value.Kind}' to '{targetType}'.", exception);
        }
    }

    private static CSharpStatus ConvertToValue(object? managedValue, CSharpValue* value)
    {
        *value = default;
        switch (managedValue)
        {
            case null:
                value->Kind = CSharpValueKind.Null;
                return CSharpStatus.Success;
            case bool boolean:
                value->Kind = CSharpValueKind.Boolean;
                value->Payload0 = boolean ? 1UL : 0UL;
                return CSharpStatus.Success;
            case byte or sbyte or short or int or long:
                value->Kind = CSharpValueKind.Int64;
                value->Payload0 = unchecked((ulong)Convert.ToInt64(managedValue, CultureInfo.InvariantCulture));
                return CSharpStatus.Success;
            case ushort or uint or ulong:
                value->Kind = CSharpValueKind.UInt64;
                value->Payload0 = Convert.ToUInt64(managedValue, CultureInfo.InvariantCulture);
                return CSharpStatus.Success;
            case float or double:
                value->Kind = CSharpValueKind.Double;
                value->Payload0 = unchecked((ulong)BitConverter.DoubleToInt64Bits(
                    Convert.ToDouble(managedValue, CultureInfo.InvariantCulture)));
                return CSharpStatus.Success;
            case Enum enumeration:
                return ConvertToValue(Convert.ChangeType(
                    enumeration,
                    Enum.GetUnderlyingType(enumeration.GetType()),
                    CultureInfo.InvariantCulture), value);
            case string text:
                byte[] bytes = Encoding.UTF8.GetBytes(text);
                void* allocation = NativeMemory.Alloc((nuint)bytes.Length + 1);
                if (allocation is null)
                {
                    throw new OutOfMemoryException();
                }
                bytes.CopyTo(new Span<byte>(allocation, bytes.Length));
                ((byte*)allocation)[bytes.Length] = 0;
                value->Kind = CSharpValueKind.Utf8String;
                value->Flags = CSharpValueFlags.ManagedOwned;
                value->Payload0 = (ulong)allocation;
                value->Payload1 = checked((ulong)bytes.Length);
                return CSharpStatus.Success;
            case Time time:
                return AllocateBlittableValue(time, value);
            case Angle angle:
                return AllocateBlittableValue(angle, value);
            case Vec2 vector2:
                return AllocateBlittableValue(vector2, value);
            case Vec2U32 vector2U32:
                return AllocateBlittableValue(vector2U32, value);
            case Vec3 vector3:
                return AllocateBlittableValue(vector3, value);
            case Vec4 vector4:
                return AllocateBlittableValue(vector4, value);
            case Quat quaternion:
                return AllocateBlittableValue(quaternion, value);
            case Color color:
                return AllocateBlittableValue(color, value);
            case Transform transform:
                return AllocateBlittableValue(transform, value);
            case NativeObject nativeObject:
                return ConvertNativeObjectToValue(nativeObject, value);
            case World world:
                return ConvertNativeObjectToValue(world.Native, value);
            case GameObject gameObject:
                return ConvertNativeObjectToValue(gameObject.Native, value);
            case Component component:
                return ConvertNativeObjectToValue(component.Native, value);
            default:
                throw new InvalidValueException(
                    $"Managed value type '{managedValue.GetType()}' is not supported by C# ABI value layout v2.");
        }
    }

    private static object DeserializeGeneratedMessage(CSharpValue value, Type targetType)
    {
        if (value.Kind is not (CSharpValueKind.Utf8String or CSharpValueKind.ByteSpan))
        {
            throw new InvalidValueException(
                $"Generated message '{targetType}' requires UTF-8 JSON, not C# ABI value kind '{value.Kind}'.");
        }

        if (value.Payload0 == 0 && value.Payload1 != 0)
        {
            throw new InvalidValueException(
                $"Generated message '{targetType}' has a null JSON pointer with a non-zero byte count.");
        }

        ReadOnlySpan<byte> json = new(
            (void*)value.Payload0,
            checked((int)value.Payload1));
        var options = new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true,
            TypeInfoResolver = new DefaultJsonTypeInfoResolver(),
        };
        options.Converters.Add(new GeneratedMessageJsonConverterFactory());
        try
        {
            JsonTypeInfo messageTypeInfo =
                JsonTypeInfo.CreateJsonTypeInfo(targetType, options);
            object? message = JsonSerializer.Deserialize(json, messageTypeInfo);
            return message ?? throw new InvalidValueException(
                $"UTF-8 JSON decoded to null for generated message '{targetType}'.");
        }
        catch (JsonException exception)
        {
            throw new InvalidValueException(
                $"Invalid UTF-8 JSON for generated message '{targetType}': {exception.Message}",
                exception);
        }
        catch (NotSupportedException exception)
        {
            throw new InvalidValueException(
                $"Generated message '{targetType}' cannot be deserialized from UTF-8 JSON: {exception.Message}",
                exception);
        }
    }

    private static object ConvertObjectHandle(CSharpValue value, Type targetType)
    {
        NativeObjectKind expectedKind = targetType == typeof(World)
            ? NativeObjectKind.World
            : targetType == typeof(GameObject)
                ? NativeObjectKind.GameObject
                : targetType == typeof(Component)
                    ? NativeObjectKind.Component
                    : NativeObjectKind.ReflectedObject;
        uint encodedKind = ((uint)value.Flags >> 8) & 0xFFu;
        NativeObjectKind kind = encodedKind is >= (uint)NativeObjectKind.World
            and <= (uint)NativeObjectKind.ReflectedObject
                ? (NativeObjectKind)encodedKind
                : expectedKind;
        if (targetType != typeof(NativeObject) && kind != expectedKind)
        {
            throw new InvalidValueException(
                $"C# ABI object kind '{kind}' cannot be assigned to managed type '{targetType}'.");
        }
        var nativeObject = new NativeObject(value.Payload0, value.Payload1, kind);

        if (targetType == typeof(NativeObject))
        {
            return nativeObject;
        }
        if (targetType == typeof(World))
        {
            return new World(nativeObject);
        }
        if (targetType == typeof(GameObject))
        {
            return new GameObject(nativeObject);
        }
        if (targetType == typeof(Component))
        {
            return new Component(nativeObject);
        }

        throw new InvalidValueException(
            $"C# ABI object handle cannot be converted to managed type '{targetType}'.");
    }

    private static object ConvertBlittableValue(CSharpValue value, Type targetType)
    {
        if (targetType == typeof(Time))
        {
            return ReadBlittableValue<Time>(value);
        }
        if (targetType == typeof(Angle))
        {
            return ReadBlittableValue<Angle>(value);
        }
        if (targetType == typeof(Vec2))
        {
            return ReadBlittableValue<Vec2>(value);
        }
        if (targetType == typeof(Vec2U32))
        {
            return ReadBlittableValue<Vec2U32>(value);
        }
        if (targetType == typeof(Vec3))
        {
            return ReadBlittableValue<Vec3>(value);
        }
        if (targetType == typeof(Vec4))
        {
            return ReadBlittableValue<Vec4>(value);
        }
        if (targetType == typeof(Quat))
        {
            return ReadBlittableValue<Quat>(value);
        }
        if (targetType == typeof(Color))
        {
            return ReadBlittableValue<Color>(value);
        }
        if (targetType == typeof(Transform))
        {
            return ReadBlittableValue<Transform>(value);
        }

        throw new InvalidValueException(
            $"C# ABI byte span cannot be converted to managed type '{targetType}'.");
    }

    private static T ReadBlittableValue<T>(CSharpValue value)
        where T : unmanaged
    {
        uint expectedSize = checked((uint)sizeof(T));
        if (value.Payload0 == 0 || value.Payload1 != expectedSize)
        {
            throw new InvalidValueException(
                $"C# ABI byte span for '{typeof(T)}' must contain exactly {expectedSize} bytes.");
        }

        return Unsafe.ReadUnaligned<T>((void*)value.Payload0);
    }

    private static CSharpStatus AllocateBlittableValue<T>(T managedValue, CSharpValue* value)
        where T : unmanaged
    {
        uint size = checked((uint)sizeof(T));
        void* allocation = NativeMemory.Alloc(size);
        if (allocation is null)
        {
            throw new OutOfMemoryException();
        }

        Unsafe.WriteUnaligned(allocation, managedValue);
        value->Kind = CSharpValueKind.ByteSpan;
        value->Flags = CSharpValueFlags.ManagedOwned;
        value->Payload0 = (ulong)allocation;
        value->Payload1 = size;
        return CSharpStatus.Success;
    }

    private static CSharpStatus ConvertNativeObjectToValue(
        NativeObject nativeObject,
        CSharpValue* value)
    {
        if (!nativeObject.IsValid)
        {
            value->Kind = CSharpValueKind.Null;
            return CSharpStatus.Success;
        }

        value->Kind = CSharpValueKind.ObjectHandle;
        value->Flags = (CSharpValueFlags)((uint)nativeObject.Kind << 8);
        value->Payload0 = nativeObject.Id;
        value->Payload1 = nativeObject.Generation;
        return CSharpStatus.Success;
    }

    private static string Decode(CSharpUtf8Span span) => Decode(span.Data, span.Length);

    private static string Decode(byte* data, uint length)
    {
        if (data is null || length == 0)
        {
            return string.Empty;
        }

        return Encoding.UTF8.GetString(new ReadOnlySpan<byte>(data, checked((int)length)));
    }

    private static CSharpStatus Fail(CSharpStatus status, Exception exception) =>
        Fail(status, exception.ToString());

    private static CSharpStatus Fail(CSharpStatus status, string message)
    {
        s_lastError = message;
        return status;
    }

    private static void Log(CSharpLogLevel level, string message)
    {
        if (!s_initialized || s_nativeApi.Log == null)
        {
            return;
        }

        byte[] bytes = Encoding.UTF8.GetBytes(message);
        fixed (byte* data = bytes)
        {
            var span = new CSharpUtf8Span
            {
                Data = data,
                Length = checked((uint)bytes.Length),
            };
            _ = s_nativeApi.Log(level, span);
        }
    }

    private static void TryDeleteDirectory(string path)
    {
        try
        {
            Directory.Delete(path, recursive: true);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private sealed class Generation
    {
        public Generation(
            ulong id,
            GenerationLoadContext loadContext,
            string shadowDirectory,
            IReadOnlyList<ScriptTypeDescriptor> descriptors,
            byte[] manifest)
        {
            Id = id;
            LoadContext = loadContext;
            ShadowDirectory = shadowDirectory;
            Types = descriptors.ToDictionary(descriptor => descriptor.Id);
            Manifest = manifest;
        }

        public ulong Id { get; }
        public GenerationLoadContext LoadContext { get; private set; }
        public string ShadowDirectory { get; }
        public Dictionary<ulong, ScriptTypeDescriptor> Types { get; private set; }
        public byte[] Manifest { get; private set; }

        public void ReleaseAndUnload()
        {
            GenerationLoadContext context = LoadContext;
            Types = [];
            Manifest = [];
            LoadContext = null!;
            context.Unload();
        }
    }

    private sealed record Instance(
        ulong Id,
        ulong GenerationId,
        ScriptTypeDescriptor Type,
        ComponentScript Script);

    private sealed record UnloadTicket(
        WeakReference ContextReference,
        string ShadowDirectory);

    private sealed class InvalidValueException : Exception
    {
        public InvalidValueException(string message)
            : base(message)
        {
        }

        public InvalidValueException(string message, Exception innerException)
            : base(message, innerException)
        {
        }
    }

    private sealed class GeneratedMessageJsonConverterFactory : JsonConverterFactory
    {
        public override bool CanConvert(Type typeToConvert) =>
            typeToConvert.IsDefined(typeof(GeneratedMessageAttribute), inherit: false);

        public override JsonConverter CreateConverter(
            Type typeToConvert,
            JsonSerializerOptions options)
        {
            _ = options;
            return new GeneratedMessageJsonConverter(typeToConvert);
        }
    }

    private sealed class GeneratedMessageJsonConverter : JsonConverter<object>
    {
        private readonly Type _messageType;

        public GeneratedMessageJsonConverter(Type messageType)
        {
            _messageType = messageType;
        }

        public override bool CanConvert(Type typeToConvert) =>
            typeToConvert == _messageType;

        public override object Read(
            ref Utf8JsonReader reader,
            Type typeToConvert,
            JsonSerializerOptions options)
        {
            if (reader.TokenType != JsonTokenType.StartObject)
            {
                throw new JsonException(
                    $"Generated message '{typeToConvert}' must be encoded as a JSON object.");
            }

            using JsonDocument document = JsonDocument.ParseValue(ref reader);
            object message = Activator.CreateInstance(typeToConvert)
                ?? throw new JsonException(
                    $"Generated message '{typeToConvert}' could not be default-constructed.");

            PopulateObject(document.RootElement, typeToConvert, message, options);
            return message;
        }

        private static void PopulateObject(
            JsonElement json,
            Type targetType,
            object target,
            JsonSerializerOptions options)
        {
            foreach (PropertyInfo property in targetType.GetProperties(
                         BindingFlags.Instance | BindingFlags.Public))
            {
                if (property.SetMethod is null ||
                    !TryGetJsonProperty(json, property.Name, out JsonElement value))
                {
                    continue;
                }

                property.SetValue(
                    target,
                    DeserializeProperty(value, property.PropertyType, options));
            }
        }

        public override void Write(
            Utf8JsonWriter writer,
            object value,
            JsonSerializerOptions options)
        {
            _ = writer;
            _ = value;
            _ = options;
            throw new NotSupportedException("Generated message JSON transport is receive-only.");
        }

        private static object? DeserializeProperty(
            JsonElement value,
            Type propertyType,
            JsonSerializerOptions options)
        {
            if (value.ValueKind == JsonValueKind.Null)
            {
                return Nullable.GetUnderlyingType(propertyType) is not null ||
                       !propertyType.IsValueType
                    ? null
                    : throw new JsonException(
                        $"JSON null cannot be assigned to generated message property type '{propertyType}'.");
            }

            Type effectiveType = Nullable.GetUnderlyingType(propertyType) ?? propertyType;
            if (effectiveType.IsEnum)
            {
                if (value.ValueKind == JsonValueKind.String)
                {
                    return Enum.Parse(effectiveType, value.GetString()!, ignoreCase: true);
                }

                Type underlyingType = Enum.GetUnderlyingType(effectiveType);
                object underlying = JsonSerializer.Deserialize(
                    value.GetRawText(),
                    underlyingType,
                    options) ?? throw new JsonException(
                    $"Could not decode enum value for '{effectiveType}'.");
                return Enum.ToObject(effectiveType, underlying);
            }

            if (effectiveType.Assembly.IsCollectible)
            {
                if (value.ValueKind != JsonValueKind.Object)
                {
                    throw new JsonException(
                        $"Collectible generated value '{effectiveType}' must be encoded as a JSON object.");
                }

                object nested = Activator.CreateInstance(effectiveType)
                    ?? throw new JsonException(
                        $"Collectible generated value '{effectiveType}' could not be default-constructed.");
                PopulateObject(value, effectiveType, nested, options);
                return nested;
            }

            return JsonSerializer.Deserialize(
                value.GetRawText(),
                propertyType,
                options);
        }

        private static bool TryGetJsonProperty(
            JsonElement message,
            string name,
            out JsonElement value)
        {
            foreach (JsonProperty property in message.EnumerateObject())
            {
                if (string.Equals(property.Name, name, StringComparison.OrdinalIgnoreCase))
                {
                    value = property.Value;
                    return true;
                }
            }

            value = default;
            return false;
        }
    }
}
