#include <CSharpPlugin/Hosting/CSharpHost.h>
#include <CSharpPlugin/Runtime/CSharpProjectRuntime.h>
#include <CSharpPlugin/Runtime/CSharpInstance.h>
#include <CSharpPlugin/Runtime/CSharpObjectRegistry.h>
#include <Core/Scripting/ScriptWorldModule.h>
#include <Core/Scripting/ScriptComponent.h>
#include <Core/World/World.h>
#include <Core/Scripting/ScriptAttributes.h>
#include <Core/World/Component.h>
#include <Foundation/Application/Application.h>
#include <Foundation/Configuration/Startup.h>
#include <Foundation/Logging/ConsoleWriter.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Utilities/CommandLineUtils.h>

namespace
{
  plCSharpUtf8Span Span(plStringView text) { return {text.GetStartPointer(), text.GetElementCount()}; }
  plResult Require(bool condition, plStringView message)
  {
    if (condition) return PL_SUCCESS;
    plLog::Error("C# integration test failed: {}", message);
    return PL_FAILURE;
  }
  plResult Capture(const plCSharpManagedApiV1* api, const plCSharpManagedReloadStateApiV1* stateApi, plUInt64 instance, plString& json)
  {
    plCSharpBuffer buffer;
    PL_SUCCEED_OR_RETURN(Require(stateApi->m_Capture(instance, &buffer) == plCSharpStatus::Success, "Capture private state"));
    json = plStringView(static_cast<const char*>(buffer.m_pData), buffer.m_uiSize);
    api->m_FreeBuffer(&buffer);
    return PL_SUCCESS;
  }
  plResult CheckAccumulator(plStringView json, double expected)
  {
    plRawMemoryStreamReader stream(json.GetStartPointer(), json.GetElementCount());
    plJSONReader reader;
    PL_SUCCEED_OR_RETURN(reader.Parse(stream));
    const plVariant* value = reader.GetTopLevelObject().GetValue("Accumulator");
    return Require(value != nullptr && value->ConvertTo<double>() == expected, "Updated lifecycle dispatch and retained accumulator");
  }

  /// A full borrowed-handle list releases handles of deleted objects before refusing a new one.
  plResult TestFullHandleListPrunesDeletedObjects(plWorld& ref_world, plDynamicArray<plCSharpObjectHandle>& ref_borrowed)
  {
    auto& registry = plCSharpObjectRegistry::GetSingleton();

    plDynamicArray<plGameObjectHandle> objects;
    for (plUInt32 i = 0; i < 4096; ++i)
    {
      plGameObject* pObject = nullptr;
      objects.PushBack(ref_world.CreateObject(plGameObjectDesc(), pObject));
      const plCSharpObjectHandle handle = registry.RegisterObject(pObject, plGetStaticRTTI<plGameObject>(), plCSharpNativeObjectKind::GameObject);
      PL_SUCCEED_OR_RETURN(Require(plCSharpExecutionScope::TrackBorrowedHandle(handle), "Track a handle below the limit"));
    }

    for (const plGameObjectHandle& hObject : objects)
      ref_world.DeleteObjectNow(hObject);

    plGameObject* pLate = nullptr;
    ref_world.CreateObject(plGameObjectDesc(), pLate);
    const plCSharpObjectHandle late = registry.RegisterObject(pLate, plGetStaticRTTI<plGameObject>(), plCSharpNativeObjectKind::GameObject);
    PL_SUCCEED_OR_RETURN(Require(plCSharpExecutionScope::TrackBorrowedHandle(late), "A full list made room by releasing deleted objects"));
    return Require(ref_borrowed.GetCount() == 1, "Only the live object's handle remains");
  }

  /// Handle-registry and execution-scope behaviour that needs no managed code or fixture.
  plResult TestHandleRegistry()
  {
    auto& registry = plCSharpObjectRegistry::GetSingleton();
    plDynamicArray<plCSharpObjectHandle> borrowed;
    plCSharpObjectHandle reflected;

    {
      plWorldDesc worldDesc("CSharp handle registry");
      plWorld world(worldDesc);
      PL_LOCK(world.GetWriteMarker());
      plCSharpExecutionScope scope(&world, &borrowed);

      PL_SUCCEED_OR_RETURN(TestFullHandleListPrunesDeletedObjects(world, borrowed));

      plScriptWorldModule* pModule = world.GetOrCreateModule<plScriptWorldModule>();
      reflected = registry.RegisterObject(pModule, pModule->GetDynamicRTTI(), plCSharpNativeObjectKind::ReflectedObject);
      void* pObject = nullptr;
      const plRTTI* pType = nullptr;
      PL_SUCCEED_OR_RETURN(Require(registry.ResolveObject(reflected, pObject, pType) == plCSharpStatus::Success, "A world module resolves while its world exists"));
    }

    // A raw-pointer handle must not outlive the world it was obtained in.
    {
      void* pObject = nullptr;
      const plRTTI* pType = nullptr;
      PL_SUCCEED_OR_RETURN(Require(registry.ResolveObject(reflected, pObject, pType) != plCSharpStatus::Success, "A world module handle stops resolving after its world is destroyed"));
    }

    registry.UnregisterObject(reflected);
    for (const plCSharpObjectHandle& handle : borrowed)
      registry.UnregisterObject(handle);

    // A script instance destroyed during its own call must not be restored as the current handle list
    // by any scope that ends afterwards, including a re-entrant one into the same instance.
    {
      plDynamicArray<plCSharpObjectHandle> destroyed;
      plDynamicArray<plCSharpObjectHandle> other;
      plCSharpObjectHandle probe;
      probe.m_uiValue = 1;
      probe.m_uiGeneration = 1;
      probe.m_uiKind = static_cast<plUInt32>(plCSharpNativeObjectKind::GameObject);

      plCSharpExecutionScope outer(nullptr, &destroyed);
      {
        plCSharpExecutionScope inner(nullptr, &other);
        plCSharpExecutionScope reentrant(nullptr, &destroyed);
        plCSharpExecutionScope::ForgetBorrowedHandles(&destroyed);
        PL_SUCCEED_OR_RETURN(Require(!plCSharpExecutionScope::TrackBorrowedHandle(probe), "A forgotten list is not the current one"));
      }

      PL_SUCCEED_OR_RETURN(Require(!plCSharpExecutionScope::TrackBorrowedHandle(probe), "Ending later scopes does not restore a forgotten list"));
      PL_SUCCEED_OR_RETURN(Require(destroyed.IsEmpty(), "Nothing was written into the forgotten list"));
    }

    plLog::Success("PASS: handle registry pruning, world-bound reflected handles and forgotten handle lists.");
    return PL_SUCCESS;
  }
}

class plCSharpRuntimeTests : public plApplication
{
public:
  plCSharpRuntimeTests() : plApplication("CSharpRuntimeTests") {}
  plResult BeforeCoreSystemsStartup() override
  {
    plStartup::AddApplicationTag("testframework");
    plStartup::AddApplicationTag("editorengineprocess");
    plGlobalLog::AddLogWriter(plLogWriter::Console::LogMessageHandler);
    return plApplication::BeforeCoreSystemsStartup();
  }
  Execution Run() override
  {
    SetReturnCode(Test().Succeeded() ? 0 : 1);
    return Execution::Quit;
  }
  plResult Test()
  {
    PL_SUCCEED_OR_RETURN(TestHandleRegistry());

    // The fixture-free checks above can run alone with -registryOnly.
    if (plCommandLineUtils::GetGlobalInstance()->GetBoolOption("-registryOnly"))
      return PL_SUCCESS;

    const plString root = plCommandLineUtils::GetGlobalInstance()->GetStringOption("-fixture");
    PL_SUCCEED_OR_RETURN(Require(!root.IsEmpty(), "Specify the managed BrickBreaker fixture with -fixture"));
    plCSharpHost host;
    PL_SUCCEED_OR_RETURN(Require(!host.IsInitialized(), "Host starts lazily"));
    PL_SUCCEED_OR_RETURN(host.RunM0StressTest(10));
    const plCSharpManagedApiV1* api = host.GetManagedApi();
    auto& runtime = plCSharpProjectRuntime::GetSingleton();
    plCSharpGenerationLease original, second, skipped, replacement;
    plUInt64 oldInstance = 0, newInstance = 0;
    PL_SCOPE_EXIT(
      if (oldInstance != 0) api->m_DestroyInstance(oldInstance);
      if (newInstance != 0) api->m_DestroyInstance(newInstance);
      runtime.ReleaseGeneration(original); runtime.ReleaseGeneration(second);
      runtime.ReleaseGeneration(skipped); runtime.ReleaseGeneration(replacement););
    plString descriptor;
    PL_SUCCEED_OR_RETURN(runtime.AcquireGeneration(plStringBuilder(root, "/r0/BrickBreaker.dll"), {}, original, descriptor));
    plCSharpInstanceCreateDesc create;
    create.m_uiGeneration = original.m_uiGeneration;
    create.m_uiTypeId = 0xE542A7983BCDBEEBULL;
    PL_SUCCEED_OR_RETURN(Require(api->m_CreateInstance(&create, &oldInstance) == plCSharpStatus::Success, "Create BrickBreaker native-host instance"));
    void* statePointer = nullptr;
    PL_SUCCEED_OR_RETURN(Require(api->m_QueryExtension(Span("Plasma.ReloadState"), 1, &statePointer) == plCSharpStatus::Success, "Reload-state extension"));
    const auto* stateApi = static_cast<const plCSharpManagedReloadStateApiV1*>(statePointer);
    auto activate = [api](plUInt64 instance) -> plResult {
      plCSharpValue result;
      const auto status = api->m_InvokeMethod(instance, plComponent_ScriptBaseClassFunctions::OnActivated, nullptr, 0, &result);
      api->m_ReleaseValue(&result);
      return Require(status == plCSharpStatus::Success, "Invoke generated lifecycle delegate");
    };
    plString state;
    PL_SUCCEED_OR_RETURN(activate(oldInstance));
    PL_SUCCEED_OR_RETURN(Capture(api, stateApi, oldInstance, state));
    PL_SUCCEED_OR_RETURN(CheckAccumulator(state, 1));
    PL_SUCCEED_OR_RETURN(runtime.AcquireGeneration(plStringBuilder(root, "/r2/BrickBreaker.dll"), {}, second, descriptor));
    PL_SUCCEED_OR_RETURN(Require(second.m_uiGeneration == original.m_uiGeneration, "Native runtime retained the live generation"));
    PL_SUCCEED_OR_RETURN(activate(oldInstance));
    PL_SUCCEED_OR_RETURN(Capture(api, stateApi, oldInstance, state));
    PL_SUCCEED_OR_RETURN(CheckAccumulator(state, 3));
    PL_SUCCEED_OR_RETURN(runtime.AcquireGeneration(plStringBuilder(root, "/r13/BrickBreaker.dll"), {}, skipped, descriptor));
    PL_SUCCEED_OR_RETURN(Require(skipped.m_uiGeneration == original.m_uiGeneration, "Skipped imports apply ordered deltas"));
    PL_SUCCEED_OR_RETURN(activate(oldInstance));
    PL_SUCCEED_OR_RETURN(Capture(api, stateApi, oldInstance, state));
    PL_SUCCEED_OR_RETURN(CheckAccumulator(state, 16));
    PL_SUCCEED_OR_RETURN(runtime.AcquireGeneration(plStringBuilder(root, "/r23/BrickBreaker.dll"), {}, replacement, descriptor));
    PL_SUCCEED_OR_RETURN(Require(replacement.m_uiGeneration != original.m_uiGeneration, "Structural edit loaded a full replacement"));
    create.m_uiGeneration = replacement.m_uiGeneration;
    PL_SUCCEED_OR_RETURN(Require(api->m_CreateInstance(&create, &newInstance) == plCSharpStatus::Success, "Create replacement"));
    PL_SUCCEED_OR_RETURN(Require(stateApi->m_Restore(newInstance, Span(state)) == plCSharpStatus::Success, "Restore full-reload private state"));
    plString restored;
    PL_SUCCEED_OR_RETURN(Capture(api, stateApi, newInstance, restored));
    PL_SUCCEED_OR_RETURN(Require(restored == state, "Full replacement retained gameplay state"));
    {
      // Exercise the real native script-instance adapter, including an optional Initialize
      // override and a serialized parameter still using its previous name.
      plWorldDesc worldDesc("CSharp reload migration");
      plWorld world(worldDesc);
      PL_LOCK(world.GetWriteMarker());
      plGameObject* owner = nullptr;
      plGameObjectDesc objectDesc;
      objectDesc.m_sName.Assign("Reload owner");
      world.CreateObject(objectDesc, owner);
      plScriptComponent* component = nullptr;
      plScriptComponent::CreateComponent(owner, component);
      plCSharpGenerationLease adapterLease;
      PL_SUCCEED_OR_RETURN(runtime.AcquireGeneration(plStringBuilder(root, "/r23/BrickBreaker.dll"), {}, adapterLease, descriptor));
      plSharedPtr<plCSharpClassData> classData = PL_DEFAULT_NEW(plCSharpClassData);
      classData->m_pGeneration = PL_DEFAULT_NEW(plCSharpGenerationContext, std::move(adapterLease));
      classData->m_uiTypeId = create.m_uiTypeId;
      classData->m_sManagedName = "Game.Scripts.BrickBreaker";
      auto& field = classData->m_Fields.ExpandAndGetRef();
      field.m_sName.Assign("RunMechanicsChecks");
      field.m_sManagedType = "System.Boolean";
      const char* identity = "pl-csharp/field/v1:Game.Scripts.BrickBreaker|RunMechanicsChecks";
      field.m_uiId = 14695981039346656037ULL;
      for (const char* ch = identity; *ch; ++ch) { field.m_uiId ^= static_cast<plUInt8>(*ch); field.m_uiId *= 1099511628211ULL; }
      field.m_FormerNames.ExpandAndGetRef().Assign("OldRunChecks");
      plCSharpInstance adapter(*component, &world, classData);
      PL_SUCCEED_OR_RETURN(Require(adapter.IsValid(), "Create native script adapter"));
      plArrayMap<plHashedString, plVariant> parameters, reloadState;
      plHashedString oldName; oldName.Assign("OldRunChecks");
      parameters.Insert(oldName, true);
      plHashedString stateName; stateName.Assign("__PlasmaReloadStateJson");
      reloadState.Insert(stateName, state);
      PL_SUCCEED_OR_RETURN(adapter.PrepareForReload(parameters, reloadState));
      PL_SUCCEED_OR_RETURN(Require(adapter.GetInstanceVariable(field.m_sName).Get<bool>(), "Former inspector name restored its current field"));
      parameters.Insert(field.m_sName, false);
      PL_SUCCEED_OR_RETURN(adapter.PrepareForReload(parameters, reloadState));
      PL_SUCCEED_OR_RETURN(Require(!adapter.GetInstanceVariable(field.m_sName).Get<bool>(), "Explicit current-name override wins over a former name"));
      plArrayMap<plHashedString, plVariant> captured;
      adapter.GetInstanceVariables(captured);
      const plUInt32 stateIndex = captured.Find(stateName);
      PL_SUCCEED_OR_RETURN(Require(stateIndex != plInvalidIndex && captured.GetValue(stateIndex).Get<plString>() == state, "Native adapter migrated exact private gameplay state"));
    }
    {
      // Identical gameplay PE/MVID must never allow an update to reuse stale dependencies.
      const plStringBuilder sourceRoot(root, "/r23");
      const plStringBuilder changedRoot(root, "/dependency-change");
      PL_SUCCEED_OR_RETURN(plOSFile::CopyFolder(sourceRoot, changedRoot));
      const plStringBuilder bindingFile(changedRoot, "/Plasma.ProjectBindings.dll");
      plOSFile changedBinding;
      PL_SUCCEED_OR_RETURN(changedBinding.Open(bindingFile, plFileOpenMode::Append));
      const plUInt8 padding = 0; // Valid PE overlay; only dependency bytes change.
      PL_SUCCEED_OR_RETURN(changedBinding.Write(&padding, sizeof(padding)));
      changedBinding.Close();
      plCSharpGenerationLease dependencyLease;
      PL_SCOPE_EXIT(runtime.ReleaseGeneration(dependencyLease));
      PL_SUCCEED_OR_RETURN(runtime.AcquireGeneration(plStringBuilder(changedRoot, "/BrickBreaker.dll"), {}, dependencyLease, descriptor));
      PL_SUCCEED_OR_RETURN(Require(dependencyLease.m_uiGeneration != replacement.m_uiGeneration, "Dependency-only edit required a full generation"));
    }
    const plUInt64 previousGeneration = original.m_uiGeneration;
    api->m_DestroyInstance(oldInstance); oldInstance = 0;
    runtime.ReleaseGeneration(original); runtime.ReleaseGeneration(second);
    plCSharpBuffer manifest;
    PL_SUCCEED_OR_RETURN(Require(api->m_GetGenerationDescriptor(previousGeneration, &manifest) == plCSharpStatus::Success, "Previous-key leases retain ownership"));
    api->m_FreeBuffer(&manifest);
    runtime.ReleaseGeneration(skipped);
    PL_SUCCEED_OR_RETURN(Require(api->m_GetGenerationDescriptor(previousGeneration, &manifest) == plCSharpStatus::GenerationNotFound, "Last lease retired its context"));
    plLog::Success("PASS: native .NET host, 10 M0 cycles, in-place updates, skipped imports, full reload state migration and generation lease retirement.");
    return PL_SUCCESS;
  }
};

PL_CONSOLEAPP_ENTRY_POINT(plCSharpRuntimeTests);
