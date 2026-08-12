#include <CSharpPlugin/CSharpPluginPCH.h>

#include <CSharpPlugin/Hosting/CSharpHost.h>
#include <CSharpPlugin/Runtime/CSharpBindingRuntime.h>
#include <CSharpPlugin/Runtime/CSharpConsoleRegistry.h>
#include <CSharpPlugin/Runtime/CSharpDebugApi.h>
#include <CSharpPlugin/Runtime/CSharpFunctionProperty.h>
#include <CSharpPlugin/Runtime/CSharpObjectRegistry.h>
#include <Core/World/Component.h>
#include <Core/World/GameObject.h>
#include <Core/Scripting/ScriptAttributes.h>
#include <Core/World/World.h>
#include <Core/World/WorldDesc.h>
#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <Foundation/Configuration/Startup.h>
#include <Foundation/Communication/Message.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <Foundation/Strings/StringConversion.h>
#include <Foundation/Utilities/CommandLineOptions.h>

#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>

#include <bit>

namespace
{
  /// \brief The directory this plugin's own DLL was loaded from, or empty if that cannot be determined.
  ///
  /// The C# payload - the managed assemblies, and the optional private .NET runtime - ships beside
  /// the plugin. Where that is depends on how the plugin got here: in an engine build it is the
  /// application directory, but installed from a package it is <store>/<id>/<version>. Asking the
  /// module where it lives covers both without either layout having to know about the other.
  plStringBuilder GetPluginDirectory()
  {
    HMODULE hSelf = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&GetPluginDirectory), &hSelf) == 0 ||
        hSelf == nullptr)
    {
      return plStringBuilder();
    }

    wchar_t szPath[4096] = {};
    const DWORD uiLength = GetModuleFileNameW(hSelf, szPath, PL_ARRAY_SIZE(szPath));
    if (uiLength == 0 || uiLength >= PL_ARRAY_SIZE(szPath))
    {
      return plStringBuilder();
    }

    plStringBuilder sPath(plStringUtf8(szPath).GetData());
    sPath.PathParentDirectory();
    sPath.MakeCleanPath();
    return sPath;
  }

  /// \brief Finds the directory holding the "CSharp" payload folder.
  ///
  /// Beside the plugin first, then upwards, then the application directory.
  ///
  /// Walking up is what makes this work for an installed package. plPlugin loads a DLL from a copy
  /// at "<plugin dir>/loaded/<N>/<name>.dll" so the original stays writable, which puts the running
  /// module two directories below its own payload. Checking only beside the module found nothing,
  /// fell through to the application directory - where an installed package has never put anything -
  /// and every script failed with "C# managed bootstrap is missing".
  plStringBuilder FindPayloadRoot()
  {
    constexpr plUInt32 uiMaxParentLevels = 4;

    plStringBuilder sDirectory = GetPluginDirectory();
    for (plUInt32 uiLevel = 0; uiLevel < uiMaxParentLevels && !sDirectory.IsEmpty(); ++uiLevel)
    {
      plStringBuilder sCandidate(sDirectory);
      sCandidate.AppendPath("CSharp");

      if (plOSFile::ExistsDirectory(sCandidate))
      {
        return sDirectory;
      }

      const plStringBuilder sPrevious = sDirectory;
      sDirectory.PathParentDirectory();
      sDirectory.MakeCleanPath();

      if (sDirectory == sPrevious)
        break;
    }

    plStringBuilder sAppDir = plOSFile::GetApplicationDirectory();
    sAppDir.MakeCleanPath();
    return sAppDir;
  }
} // namespace

struct plScriptExtensionClass_CSharpM0
{
  static plInt64 Probe(plInt64 iInput) { return iInput + 1; }
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NO_LINKAGE, plScriptExtensionClass_CSharpM0);

// clang-format off
PL_BEGIN_STATIC_REFLECTED_TYPE(plScriptExtensionClass_CSharpM0, plNoBase, 1, plRTTINoAllocator)
{
  PL_BEGIN_FUNCTIONS
  {
    PL_SCRIPT_FUNCTION_PROPERTY(Probe, In, "Input"),
  }
  PL_END_FUNCTIONS;
  PL_BEGIN_ATTRIBUTES
  {
    new plScriptExtensionAttribute("CSharpM0"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_STATIC_REFLECTED_TYPE;
// clang-format on

PL_IMPLEMENT_SINGLETON(plCSharpHost);

namespace
{
  static plCommandLineOptionBool s_CSharpM0Stress(
    "CSharp", "-csharpM0Stress", "Runs the hosted .NET load/invoke/unload M0 stress probe at startup.", false);
  static plCommandLineOptionInt s_CSharpM0Cycles(
    "CSharp", "-csharpM0Cycles", "Number of C# M0 stress cycles.", 1000, 1, 100000);

  static plStringView GetCSharpStatusName(plCSharpStatus status)
  {
    switch (status)
    {
      case plCSharpStatus::Success:
        return "Success";
      case plCSharpStatus::InvalidArgument:
        return "Invalid argument";
      case plCSharpStatus::AbiMismatch:
        return "Runtime interface mismatch";
      case plCSharpStatus::NotInitialized:
        return "C# runtime not initialized";
      case plCSharpStatus::RuntimeLoadFailed:
        return "C# runtime failed to load";
      case plCSharpStatus::AssemblyLoadFailed:
        return "Script assembly failed to load";
      case plCSharpStatus::GenerationNotFound:
        return "Compiled script generation not found";
      case plCSharpStatus::GenerationInUse:
        return "Compiled script generation is still in use";
      case plCSharpStatus::TypeNotFound:
        return "C# script type not found";
      case plCSharpStatus::InstanceNotFound:
        return "C# script instance not found";
      case plCSharpStatus::MemberNotFound:
        return "Engine API member not found";
      case plCSharpStatus::InvalidValue:
        return "Unsupported or invalid value";
      case plCSharpStatus::ManagedException:
        return "C# exception";
      case plCSharpStatus::UnloadIncomplete:
        return "C# scripts could not be fully unloaded";
      case plCSharpStatus::BufferTooSmall:
        return "Communication buffer too small";
      case plCSharpStatus::Unsupported:
        return "Operation not supported";
    }

    return "Unknown C# error";
  }

  static void HOSTFXR_CALLTYPE HostFxrErrorWriter(const char_t* szMessage)
  {
    if (szMessage != nullptr)
    {
      plLog::Error(".NET hostfxr: {}", plStringUtf8(szMessage).GetData());
    }
  }

  static plCSharpUtf8Span MakeUtf8Span(plStringView sText)
  {
    plCSharpUtf8Span result;
    result.m_pData = sText.GetStartPointer();
    result.m_uiLength = sText.GetElementCount();
    return result;
  }

  static plStringView GetUtf8View(plCSharpUtf8Span span)
  {
    return span.m_pData == nullptr
             ? plStringView()
             : plStringView(span.m_pData, span.m_uiLength);
  }

  static bool ReadJsonNumber(
    const plVariantDictionary& dictionary, plStringView sName, float& out_fValue)
  {
    const plVariant* pValue = dictionary.GetValue(sName);
    if (pValue == nullptr || !pValue->IsNumber())
      return false;
    out_fValue = pValue->ConvertTo<float>();
    return true;
  }

  static bool ReadJsonUInt32(
    const plVariantDictionary& dictionary, plStringView sName, plUInt32& out_uiValue)
  {
    const plVariant* pValue = dictionary.GetValue(sName);
    if (pValue == nullptr || !pValue->IsNumber())
      return false;

    const plUInt64 uiValue = pValue->ConvertTo<plUInt64>();
    if (uiValue > plMath::MaxValue<plUInt32>())
      return false;

    out_uiValue = static_cast<plUInt32>(uiValue);
    return true;
  }

  static plResult ConvertJsonProperty(
    const plVariant& jsonValue, const plRTTI* pTargetType, plVariant& out_value)
  {
    if (pTargetType == nullptr)
      return PL_FAILURE;

    if (jsonValue.IsA<plVariantDictionary>())
    {
      const plVariantDictionary& dictionary = jsonValue.Get<plVariantDictionary>();
      float x = 0;
      float y = 0;
      float z = 0;
      float w = 0;
      plUInt32 xU32 = 0;
      plUInt32 yU32 = 0;
      switch (pTargetType->GetVariantType())
      {
        case plVariantType::Vector2:
          if (ReadJsonNumber(dictionary, "X", x) &&
              ReadJsonNumber(dictionary, "Y", y))
          {
            out_value = plVec2(x, y);
            return PL_SUCCESS;
          }
          break;
        case plVariantType::Vector2U:
          if (ReadJsonUInt32(dictionary, "X", xU32) &&
              ReadJsonUInt32(dictionary, "Y", yU32))
          {
            out_value = plVec2U32(xU32, yU32);
            return PL_SUCCESS;
          }
          break;
        case plVariantType::Vector3:
          if (ReadJsonNumber(dictionary, "X", x) &&
              ReadJsonNumber(dictionary, "Y", y) &&
              ReadJsonNumber(dictionary, "Z", z))
          {
            out_value = plVec3(x, y, z);
            return PL_SUCCESS;
          }
          break;
        case plVariantType::Vector4:
          if (ReadJsonNumber(dictionary, "X", x) &&
              ReadJsonNumber(dictionary, "Y", y) &&
              ReadJsonNumber(dictionary, "Z", z) &&
              ReadJsonNumber(dictionary, "W", w))
          {
            out_value = plVec4(x, y, z, w);
            return PL_SUCCESS;
          }
          break;
        case plVariantType::Color:
        case plVariantType::ColorGamma:
          if (ReadJsonNumber(dictionary, "R", x) &&
              ReadJsonNumber(dictionary, "G", y) &&
              ReadJsonNumber(dictionary, "B", z))
          {
            if (!ReadJsonNumber(dictionary, "A", w))
              w = 1.0f;
            const plColor color(x, y, z, w);
            out_value = pTargetType->GetVariantType() == plVariantType::ColorGamma
                          ? plVariant(plColorGammaUB(color))
                          : plVariant(color);
            return PL_SUCCESS;
          }
          break;
        default:
          break;
      }
      return PL_FAILURE;
    }

    const plVariantType::Enum targetType = pTargetType->GetVariantType();
    if (targetType == plVariantType::Invalid)
    {
      out_value = jsonValue;
      return PL_SUCCESS;
    }

    plResult conversionResult = PL_FAILURE;
    out_value = jsonValue.ConvertTo(targetType, &conversionResult);
    return conversionResult;
  }

  static plResult PopulateMessageFromJson(
    plMessage& message, plStringView sPayloadJson)
  {
    plRawMemoryStreamReader stream(
      sPayloadJson.GetStartPointer(), sPayloadJson.GetElementCount());
    plJSONReader reader;
    if (reader.Parse(stream).Failed() ||
        reader.GetTopLevelElementType() != plJSONReader::ElementType::Dictionary)
    {
      return PL_FAILURE;
    }

    const plVariantDictionary& values = reader.GetTopLevelObject();
    plDynamicArray<const plAbstractProperty*> properties;
    message.GetDynamicRTTI()->GetAllProperties(properties);
    for (const plAbstractProperty* pProperty : properties)
    {
      if (pProperty->GetCategory() != plPropertyCategory::Member ||
          pProperty->GetFlags().IsSet(plPropertyFlags::ReadOnly))
      {
        continue;
      }

      plStringView sPropertyName = pProperty->GetPropertyName();
      sPropertyName.TrimWordStart("m_");
      const plVariant* pJsonValue = values.GetValue(sPropertyName);
      if (pJsonValue == nullptr)
        continue;

      plVariant propertyValue;
      if (ConvertJsonProperty(
            *pJsonValue, pProperty->GetSpecificType(), propertyValue)
            .Failed())
      {
        return PL_FAILURE;
      }

      plReflectionUtils::SetMemberPropertyValue(
        static_cast<const plAbstractMemberProperty*>(pProperty),
        &message, propertyValue);
    }
    return PL_SUCCESS;
  }

  static plUInt64 StableHash(plStringView sText)
  {
    constexpr plUInt64 uiOffset = 14695981039346656037ULL;
    constexpr plUInt64 uiPrime = 1099511628211ULL;

    plUInt64 uiHash = uiOffset;
    for (const char* pByte = sText.GetStartPointer(); pByte < sText.GetEndPointer(); ++pByte)
    {
      uiHash ^= static_cast<plUInt8>(*pByte);
      uiHash *= uiPrime;
    }
    return uiHash;
  }

  static bool IsHostSuccess(plInt32 iResult)
  {
    return iResult >= 0;
  }
} // namespace

// clang-format off
PL_BEGIN_SUBSYSTEM_DECLARATION(CSharpPlugin, CSharpHost)

BEGIN_SUBSYSTEM_DEPENDENCIES
  "Foundation"
END_SUBSYSTEM_DEPENDENCIES

ON_HIGHLEVELSYSTEMS_STARTUP
{
  plCSharpHost* pHost = PL_DEFAULT_NEW(plCSharpHost);
  if (s_CSharpM0Stress.GetOptionValue(plCommandLineOption::LogMode::AlwaysIfSpecified))
  {
    const plUInt32 uiCycles = static_cast<plUInt32>(
      s_CSharpM0Cycles.GetOptionValue(plCommandLineOption::LogMode::AlwaysIfSpecified));
    if (pHost->RunM0StressTest(uiCycles).Failed())
    {
      plLog::Error("C# M0 stress probe failed.");
    }
  }
}

ON_HIGHLEVELSYSTEMS_SHUTDOWN
{
  plCSharpHost* pHost = plCSharpHost::GetSingleton();
  PL_DEFAULT_DELETE(pHost);
}

PL_END_SUBSYSTEM_DECLARATION;
// clang-format on

plCSharpHost::plCSharpHost()
  : m_SingletonRegistrar(this)
{
  m_NativeApi.m_Log = &plCSharpHost::NativeLog;
  m_NativeApi.m_ValidateObject = &plCSharpHost::NativeValidateObject;
  m_NativeApi.m_InvokeReflected = &plCSharpHost::NativeInvokeReflected;
  m_NativeApi.m_ReleaseNativeValue = &plCSharpHost::NativeReleaseValue;
  m_NativeApi.m_QueryExtension = &plCSharpHost::NativeQueryExtension;
  m_NativeApi.m_M0Probe = &plCSharpHost::NativeM0Probe;
}

plCSharpHost::~plCSharpHost()
{
  Shutdown();
  plCSharpBindingRuntime::GetSingleton().Shutdown();

  // CoreCLR and hostfxr are process-lifetime once initialized. Do not FreeLibrary hostfxr here.
  m_pHostFxrModule = nullptr;
}

plResult plCSharpHost::Initialize(plStringView sManagedRoot)
{
  PL_LOCK(m_InitializationMutex);

  if (m_InitializationState == InitializationState::Initialized)
    return PL_SUCCESS;
  if (m_InitializationState != InitializationState::Uninitialized)
    return PL_FAILURE;

  if (plCSharpBindingRuntime::GetSingleton().Startup().Failed())
  {
    plLog::Error("C# scripting could not initialize the reflected binding runtime.");
    m_InitializationState = InitializationState::Failed;
    return PL_FAILURE;
  }

  PL_PROFILE_SCOPE("CSharp.InitializeCoreCLR");
  plResult result = PL_FAILURE;
  if (sManagedRoot.IsEmpty())
  {
    plStringBuilder sDefaultRoot = FindPayloadRoot();
    sDefaultRoot.AppendPath("CSharp");
    result = LoadRuntimeAndBootstrap(sDefaultRoot);
  }
  else
  {
    result = LoadRuntimeAndBootstrap(sManagedRoot);
  }

  m_InitializationState =
    result.Succeeded() ? InitializationState::Initialized : InitializationState::Failed;
  return result;
}

void plCSharpHost::Shutdown()
{
  PL_LOCK(m_InitializationMutex);
  if (m_InitializationState == InitializationState::ShuttingDown)
    return;

  const bool bWasInitialized =
    m_InitializationState == InitializationState::Initialized;
  m_InitializationState = InitializationState::ShuttingDown;

  if (bWasInitialized && m_ManagedApi.m_Shutdown != nullptr)
  {
    const plCSharpStatus status = m_ManagedApi.m_Shutdown();
    if (status != plCSharpStatus::Success)
    {
      LogManagedError("shutdown", status);
    }
  }

  m_ManagedApi = {};
}

bool plCSharpHost::IsInitialized() const
{
  PL_LOCK(m_InitializationMutex);
  return m_InitializationState == InitializationState::Initialized;
}

const plCSharpManagedApiV1* plCSharpHost::GetManagedApi() const
{
  PL_LOCK(m_InitializationMutex);
  return m_InitializationState == InitializationState::Initialized ? &m_ManagedApi : nullptr;
}

plResult plCSharpHost::LoadRuntimeAndBootstrap(plStringView sManagedRoot)
{
  plStringBuilder sRuntimeConfig(sManagedRoot);
  sRuntimeConfig.AppendPath("Plasma.ManagedHost.runtimeconfig.json");

  plStringBuilder sManagedAssembly(sManagedRoot);
  sManagedAssembly.AppendPath("Plasma.ManagedHost.dll");

  if (!plOSFile::ExistsFile(sRuntimeConfig) || !plOSFile::ExistsFile(sManagedAssembly))
  {
    plLog::Error("C# managed bootstrap is missing. Expected '{}' and '{}'.", sRuntimeConfig, sManagedAssembly);
    return PL_FAILURE;
  }

  // DotNet sits beside the CSharp folder in every layout, so it is derived from the managed root
  // rather than looked up independently - that keeps the two halves of the payload together
  // whether they came from the application directory, a package store, or an explicit root.
  plStringBuilder sPrivateDotNetRoot(sManagedRoot);
  sPrivateDotNetRoot.PathParentDirectory();
  sPrivateDotNetRoot.AppendPath("DotNet");
  sPrivateDotNetRoot.MakeCleanPath();

  plStringBuilder sPrivateHostFxr(sPrivateDotNetRoot);
  sPrivateHostFxr.AppendPath("host/fxr/10.0.10/hostfxr.dll");
  const bool bUsePrivateRuntime = plOSFile::ExistsFile(sPrivateHostFxr);

  plStringWChar privateDotNetRootWide(sPrivateDotNetRoot);
  get_hostfxr_parameters hostFxrParameters = {};
  hostFxrParameters.size = sizeof(hostFxrParameters);
  hostFxrParameters.dotnet_root = bUsePrivateRuntime ? privateDotNetRootWide.GetData() : nullptr;

  wchar_t szHostFxrPath[4096] = {};
  size_t uiHostFxrPathLength = PL_ARRAY_SIZE(szHostFxrPath);
  const plInt32 iPathResult =
    get_hostfxr_path(szHostFxrPath, &uiHostFxrPathLength, bUsePrivateRuntime ? &hostFxrParameters : nullptr);
  if (iPathResult != 0)
  {
    plLog::Error("C# host could not resolve hostfxr (0x{:08X}). Install .NET 10 or deploy the private DotNet runtime.",
      static_cast<plUInt32>(iPathResult));
    return PL_FAILURE;
  }

  HMODULE pHostFxr = LoadLibraryW(szHostFxrPath);
  if (pHostFxr == nullptr)
  {
    plLog::Error("C# host failed to load hostfxr from '{}' (Win32 error {}).",
      plStringUtf8(szHostFxrPath).GetData(), static_cast<plUInt32>(GetLastError()));
    return PL_FAILURE;
  }
  m_pHostFxrModule = pHostFxr;

  const auto initializeForConfig = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
    GetProcAddress(pHostFxr, "hostfxr_initialize_for_runtime_config"));
  const auto getRuntimeDelegate =
    reinterpret_cast<hostfxr_get_runtime_delegate_fn>(GetProcAddress(pHostFxr, "hostfxr_get_runtime_delegate"));
  const auto closeHostContext = reinterpret_cast<hostfxr_close_fn>(GetProcAddress(pHostFxr, "hostfxr_close"));
  const auto setErrorWriter =
    reinterpret_cast<hostfxr_set_error_writer_fn>(GetProcAddress(pHostFxr, "hostfxr_set_error_writer"));

  if (initializeForConfig == nullptr || getRuntimeDelegate == nullptr || closeHostContext == nullptr)
  {
    plLog::Error("C# hostfxr at '{}' does not expose the required hosting APIs.",
      plStringUtf8(szHostFxrPath).GetData());
    FreeLibrary(pHostFxr);
    m_pHostFxrModule = nullptr;
    return PL_FAILURE;
  }

  hostfxr_error_writer_fn pPreviousErrorWriter = nullptr;
  if (setErrorWriter != nullptr)
  {
    pPreviousErrorWriter = setErrorWriter(&HostFxrErrorWriter);
  }

  plStringWChar runtimeConfigWide(sRuntimeConfig);
  plStringWChar applicationPathWide(plOSFile::GetApplicationPath());
  hostfxr_initialize_parameters initializeParameters = {};
  initializeParameters.size = sizeof(initializeParameters);
  initializeParameters.host_path = applicationPathWide.GetData();
  initializeParameters.dotnet_root = bUsePrivateRuntime ? privateDotNetRootWide.GetData() : nullptr;

  hostfxr_handle pHostContext = nullptr;
  const plInt32 iInitializeResult = initializeForConfig(
    runtimeConfigWide.GetData(), &initializeParameters, &pHostContext);
  if (!IsHostSuccess(iInitializeResult) || pHostContext == nullptr)
  {
    if (setErrorWriter != nullptr)
    {
      setErrorWriter(pPreviousErrorWriter);
    }
    plLog::Error("C# hostfxr failed to initialize '{}' (0x{:08X}).",
      sRuntimeConfig, static_cast<plUInt32>(iInitializeResult));
    if (pHostContext != nullptr)
      closeHostContext(pHostContext);
    FreeLibrary(pHostFxr);
    m_pHostFxrModule = nullptr;
    return PL_FAILURE;
  }

  load_assembly_and_get_function_pointer_fn loadAssembly = nullptr;
  const plInt32 iDelegateResult = getRuntimeDelegate(
    pHostContext,
    hdt_load_assembly_and_get_function_pointer,
    reinterpret_cast<void**>(&loadAssembly));
  closeHostContext(pHostContext);

  if (setErrorWriter != nullptr)
  {
    setErrorWriter(pPreviousErrorWriter);
  }

  if (!IsHostSuccess(iDelegateResult) || loadAssembly == nullptr)
  {
    plLog::Error("C# hostfxr could not provide load_assembly_and_get_function_pointer (0x{:08X}).",
      static_cast<plUInt32>(iDelegateResult));
    return PL_FAILURE;
  }

  plCSharpManagedInitializeFn initializeManaged = nullptr;
  plStringWChar managedAssemblyWide(sManagedAssembly);
  const plInt32 iLoadResult = loadAssembly(
    managedAssemblyWide.GetData(),
    L"Plasma.ManagedHost.Bootstrap, Plasma.ManagedHost",
    L"Initialize",
    UNMANAGEDCALLERSONLY_METHOD,
    nullptr,
    reinterpret_cast<void**>(&initializeManaged));
  if (iLoadResult != 0 || initializeManaged == nullptr)
  {
    plLog::Error("C# host could not load the managed bootstrap entry point (0x{:08X}).",
      static_cast<plUInt32>(iLoadResult));
    return PL_FAILURE;
  }

  m_ManagedApi = {};
  m_ManagedApi.m_uiSize = sizeof(m_ManagedApi);
  m_ManagedApi.m_uiVersion = PL_CSHARP_ABI_VERSION;
  m_ManagedApi.m_uiPointerSize = sizeof(void*);
  m_ManagedApi.m_uiValueLayoutVersion = PL_CSHARP_VALUE_LAYOUT_VERSION;

  const plCSharpStatus status = initializeManaged(&m_NativeApi, &m_ManagedApi);
  if (status != plCSharpStatus::Success)
  {
    plLog::Error("C# managed bootstrap rejected the native ABI with status {}.",
      static_cast<plInt32>(status));
    if (m_ManagedApi.m_Shutdown != nullptr)
      m_ManagedApi.m_Shutdown();
    m_ManagedApi = {};
    return PL_FAILURE;
  }

  const bool bHasRequiredFunctions =
    m_ManagedApi.m_Shutdown != nullptr &&
    m_ManagedApi.m_LoadGeneration != nullptr &&
    m_ManagedApi.m_GetGenerationDescriptor != nullptr &&
    m_ManagedApi.m_CreateInstance != nullptr &&
    m_ManagedApi.m_DestroyInstance != nullptr &&
    m_ManagedApi.m_InvokeMethod != nullptr &&
    m_ManagedApi.m_GetField != nullptr &&
    m_ManagedApi.m_SetField != nullptr &&
    m_ManagedApi.m_DispatchMessage != nullptr &&
    m_ManagedApi.m_UnloadGeneration != nullptr &&
    m_ManagedApi.m_GetLastError != nullptr &&
    m_ManagedApi.m_FreeBuffer != nullptr &&
    m_ManagedApi.m_ReleaseValue != nullptr &&
    m_ManagedApi.m_QueryExtension != nullptr;

  if (m_ManagedApi.m_uiVersion != PL_CSHARP_ABI_VERSION ||
      m_ManagedApi.m_uiPointerSize != sizeof(void*) ||
      m_ManagedApi.m_uiValueLayoutVersion != PL_CSHARP_VALUE_LAYOUT_VERSION ||
      m_ManagedApi.m_uiSize < PL_CSHARP_MANAGED_API_V1_REQUIRED_SIZE ||
      !bHasRequiredFunctions)
  {
    plLog::Error("C# managed bootstrap returned an incompatible API table.");
    if (m_ManagedApi.m_Shutdown != nullptr)
      m_ManagedApi.m_Shutdown();
    m_ManagedApi = {};
    return PL_FAILURE;
  }

  plLog::Info("C# scripting initialized .NET through '{}' using {} runtime resolution.",
    plStringUtf8(szHostFxrPath).GetData(), bUsePrivateRuntime ? "private" : "registered");
  return PL_SUCCESS;
}

plResult plCSharpHost::RunM0StressTest(plUInt32 uiCycles)
{
  if (Initialize().Failed() || uiCycles == 0 ||
      m_ManagedApi.m_uiSize < sizeof(plCSharpManagedApiV1) ||
      m_ManagedApi.m_RunM0Probe == nullptr)
  {
    plLog::Error("The optional C# M0 ABI probe is unavailable.");
    return PL_FAILURE;
  }

  plStringBuilder sAssemblyPath = plOSFile::GetApplicationDirectory();
  sAssemblyPath.AppendPath("CSharp/M0Game/Plasma.ManagedM0Game.dll");
  if (!plOSFile::ExistsFile(sAssemblyPath))
  {
    plLog::Error("C# M0 game assembly is missing at '{}'.", sAssemblyPath);
    return PL_FAILURE;
  }

  const plUInt64 uiTypeId = StableHash("pl-csharp/type/v1:Plasma.ManagedM0Game.M0Script");
  const plUInt64 uiCounterFieldId =
    StableHash("pl-csharp/field/v1:Plasma.ManagedM0Game.M0Script|Counter");
  const plUInt64 uiMessageId =
    StableHash("pl-csharp/message/v1:Plasma.ManagedM0Game.M0Script|OnM0Message|plMsgCSharpM0");
  constexpr plUInt64 uiReflectedProbeId = 0x19e0103debb2f285ULL;

  plUInt64 uiBindingSchemaHash = 0;
  if (plCSharpBindingRuntime::GetSingleton().GetSchemaHash(uiBindingSchemaHash).Failed())
    return PL_FAILURE;

  plWorldDesc worldDesc("CSharpM0");
  plWorld world(worldDesc);

  plLog::Info("Running {} C# M0 load/invoke/unload cycle(s).", uiCycles);
  for (plUInt32 uiCycle = 0; uiCycle < uiCycles; ++uiCycle)
  {
    plCSharpGenerationLoadDesc loadDesc;
    loadDesc.m_AssemblyPath = MakeUtf8Span(sAssemblyPath);

    plUInt64 uiGeneration = 0;
    plCSharpStatus status = m_ManagedApi.m_LoadGeneration(&loadDesc, &uiGeneration);
    if (status != plCSharpStatus::Success)
    {
      LogManagedError("load M0 generation", status);
      return PL_FAILURE;
    }

    plCSharpBuffer descriptor;
    status = m_ManagedApi.m_GetGenerationDescriptor(uiGeneration, &descriptor);
    if (status != plCSharpStatus::Success || descriptor.m_uiSize == 0)
    {
      LogManagedError("read M0 generation descriptor", status);
      return PL_FAILURE;
    }
    m_ManagedApi.m_FreeBuffer(&descriptor);

    plCSharpInstanceCreateDesc createDesc;
    createDesc.m_uiGeneration = uiGeneration;
    createDesc.m_uiTypeId = uiTypeId;
    createDesc.m_Owner = {0x1001, 1, 2, 0};
    createDesc.m_World = {0x2001, 1, 1, 0};
    createDesc.m_OwnerComponent = {0x3001, 1, 3, 0};

    plUInt64 uiInstance = 0;
    status = m_ManagedApi.m_CreateInstance(&createDesc, &uiInstance);
    if (status != plCSharpStatus::Success)
    {
      LogManagedError("create M0 instance", status);
      return PL_FAILURE;
    }

    plCSharpValue result;
    {
      plCSharpExecutionScope scope(&world);
      status = m_ManagedApi.m_InvokeMethod(uiInstance, 0, nullptr, 0, &result);
    }
    if (status != plCSharpStatus::Success)
    {
      LogManagedError("invoke M0 Initialize", status);
      return PL_FAILURE;
    }

    plCSharpValue counterValue;
    status = m_ManagedApi.m_GetField(uiInstance, uiCounterFieldId, &counterValue);
    if (status != plCSharpStatus::Success ||
        counterValue.m_Kind != plCSharpValueKind::Int64 ||
        static_cast<plInt64>(counterValue.m_uiPayload0) != 6)
    {
      LogManagedError("read initialized M0 Counter", status);
      return PL_FAILURE;
    }
    m_ManagedApi.m_ReleaseValue(&counterValue);

    counterValue.m_Kind = plCSharpValueKind::Int64;
    counterValue.m_uiPayload0 = 10;
    status = m_ManagedApi.m_SetField(uiInstance, uiCounterFieldId, &counterValue);
    if (status != plCSharpStatus::Success)
    {
      LogManagedError("set M0 Counter", status);
      return PL_FAILURE;
    }

    plCSharpValue updateArgument;
    updateArgument.m_Kind = plCSharpValueKind::Double;
    updateArgument.m_uiPayload0 = std::bit_cast<plUInt64>(3.0);
    status = m_ManagedApi.m_InvokeMethod(uiInstance, 5, &updateArgument, 1, &result);
    if (status != plCSharpStatus::Success)
    {
      LogManagedError("invoke M0 Update", status);
      return PL_FAILURE;
    }

    plCSharpValue messagePayload;
    constexpr plStringView sMessageJson = R"({"Amount":4})";
    messagePayload.m_Kind = plCSharpValueKind::Utf8String;
    messagePayload.m_uiPayload0 =
      reinterpret_cast<plUInt64>(sMessageJson.GetStartPointer());
    messagePayload.m_uiPayload1 = sMessageJson.GetElementCount();
    status = m_ManagedApi.m_DispatchMessage(uiInstance, uiMessageId, &messagePayload);
    if (status != plCSharpStatus::Success)
    {
      LogManagedError("dispatch M0 message", status);
      return PL_FAILURE;
    }

    status = m_ManagedApi.m_GetField(uiInstance, uiCounterFieldId, &counterValue);
    if (status != plCSharpStatus::Success ||
        counterValue.m_Kind != plCSharpValueKind::Int64 ||
        static_cast<plInt64>(counterValue.m_uiPayload0) != 17)
    {
      LogManagedError("verify M0 Counter", status);
      return PL_FAILURE;
    }
    m_ManagedApi.m_ReleaseValue(&counterValue);

    plInt64 iProbeResult = 0;
    status = m_ManagedApi.m_RunM0Probe(41, &iProbeResult);
    if (status != plCSharpStatus::Success || iProbeResult != 42)
    {
      LogManagedError("run M0 managed-to-native probe", status);
      return PL_FAILURE;
    }

    status = m_ManagedApi.m_DestroyInstance(uiInstance);
    if (status != plCSharpStatus::Success)
    {
      LogManagedError("destroy M0 instance", status);
      return PL_FAILURE;
    }

    plCSharpUnloadReport unloadReport;
    status = m_ManagedApi.m_UnloadGeneration(uiGeneration, &unloadReport);
    if (status != plCSharpStatus::Success || unloadReport.m_bLoadContextAlive != 0)
    {
      LogManagedError("unload M0 generation", status);
      return PL_FAILURE;
    }

    plUInt64 uiStaleInstance = 0;
    status = m_ManagedApi.m_CreateInstance(&createDesc, &uiStaleInstance);
    if (status != plCSharpStatus::GenerationNotFound)
    {
      plLog::Error("C# M0 stale generation {} was not rejected (status {}).",
        uiGeneration, static_cast<plInt32>(status));
      return PL_FAILURE;
    }
  }

  plLog::Success("C# M0 completed {} load/invoke/unload cycles with stale-generation rejection.", uiCycles);
  return PL_SUCCESS;
}

void plCSharpHost::LogManagedError(plStringView sOperation, plCSharpStatus status) const
{
  const plStringView sStatusName = GetCSharpStatusName(status);
  if (m_ManagedApi.m_GetLastError == nullptr || m_ManagedApi.m_FreeBuffer == nullptr)
  {
    plLog::Error("C# {} failed. [Diagnostic: {} ({})]", sOperation, sStatusName, static_cast<plInt32>(status));
    return;
  }

  plCSharpBuffer error;
  if (m_ManagedApi.m_GetLastError(&error) == plCSharpStatus::Success && error.m_pData != nullptr)
  {
    const plStringView sMessage(static_cast<const char*>(error.m_pData), error.m_uiSize);
    plLog::Error("C# {} failed: {}\n[Diagnostic: {} ({})]", sOperation, sMessage, sStatusName, static_cast<plInt32>(status));
    m_ManagedApi.m_FreeBuffer(&error);
    return;
  }

  plLog::Error("C# {} failed. [Diagnostic: {} ({})]", sOperation, sStatusName, static_cast<plInt32>(status));
}

plCSharpStatus PL_CSHARP_CALL plCSharpHost::NativeLog(plCSharpLogLevel level, plCSharpUtf8Span message)
{
  if (message.m_pData == nullptr && message.m_uiLength != 0)
  {
    return plCSharpStatus::InvalidArgument;
  }

  const plStringView sMessage(message.m_pData, message.m_uiLength);
  switch (level)
  {
    case plCSharpLogLevel::Debug:
      plLog::Debug("C#: {}", sMessage);
      break;
    case plCSharpLogLevel::Dev:
      plLog::Dev("C#: {}", sMessage);
      break;
    case plCSharpLogLevel::Info:
      plLog::Info("C#: {}", sMessage);
      break;
    case plCSharpLogLevel::Success:
      plLog::Success("C#: {}", sMessage);
      break;
    case plCSharpLogLevel::Warning:
      plLog::Warning("C#: {}", sMessage);
      break;
    case plCSharpLogLevel::SeriousWarning:
      plLog::SeriousWarning("C#: {}", sMessage);
      break;
    case plCSharpLogLevel::Error:
      plLog::Error("C#: {}", sMessage);
      break;
    default:
      return plCSharpStatus::InvalidArgument;
  }
  return plCSharpStatus::Success;
}

plCSharpStatus PL_CSHARP_CALL plCSharpHost::NativeValidateObject(
  plCSharpObjectHandle object, plUInt32 uiExpectedKind)
{
  return plCSharpBindingRuntime::GetSingleton().ValidateObject(object, uiExpectedKind);
}

plCSharpStatus PL_CSHARP_CALL plCSharpHost::NativeInvokeReflected(
  plUInt64 uiFunctionId,
  plCSharpObjectHandle target,
  const plCSharpValue* pArguments,
  plUInt32 uiArgumentCount,
  plCSharpValue* out_pResult)
{
  return plCSharpBindingRuntime::GetSingleton().InvokeReflected(
    uiFunctionId, target, pArguments, uiArgumentCount, out_pResult);
}

plCSharpStatus PL_CSHARP_CALL plCSharpHost::NativeReleaseValue(plCSharpValue* pValue)
{
  return plCSharpBindingRuntime::GetSingleton().ReleaseValue(pValue);
}

plCSharpStatus PL_CSHARP_CALL plCSharpHost::NativeQueryExtension(
  plCSharpUtf8Span name, plUInt32 uiMinimumVersion, void** out_ppApi)
{
  if (out_ppApi == nullptr)
    return plCSharpStatus::InvalidArgument;

  *out_ppApi = nullptr;

  const plStringView sName = GetUtf8View(name);

  if (sName == "Plasma.World")
  {
    if (uiMinimumVersion > 2)
      return plCSharpStatus::Unsupported;

    static plCSharpWorldApiV1 worldApi = []()
    {
      plCSharpWorldApiV1 api;
      api.m_SendMessage = &plCSharpHost::NativeSendMessage;
      api.m_SendManagedMessage = &plCSharpHost::NativeSendManagedMessage;
      return api;
    }();
    *out_ppApi = &worldApi;
    return plCSharpStatus::Success;
  }

  if (sName == "Plasma.Debug")
  {
    if (uiMinimumVersion > 1)
      return plCSharpStatus::Unsupported;

    *out_ppApi = const_cast<plCSharpDebugApiV1*>(plCSharpDebugApi::GetApi());
    return plCSharpStatus::Success;
  }

  if (sName == "Plasma.Console")
  {
    if (uiMinimumVersion > 1)
      return plCSharpStatus::Unsupported;

    *out_ppApi = const_cast<plCSharpConsoleApiV1*>(plCSharpConsoleRegistry::GetConsoleApi());
    return plCSharpStatus::Success;
  }

  return plCSharpStatus::Unsupported;
}

plCSharpStatus PL_CSHARP_CALL plCSharpHost::NativeSendMessage(
  plCSharpObjectHandle target, plCSharpObjectHandle senderComponent,
  plCSharpUtf8Span nativeTypeName, plCSharpUtf8Span payloadJson,
  plCSharpMessageRouting routing)
{
  void* pTargetObject = nullptr;
  const plRTTI* pTargetType = nullptr;
  if (plCSharpObjectRegistry::GetSingleton().ResolveObject(
        target, pTargetObject, pTargetType) != plCSharpStatus::Success ||
      pTargetType == nullptr || !pTargetType->IsDerivedFrom<plGameObject>())
  {
    return plCSharpStatus::InvalidArgument;
  }

  auto* pGameObject = static_cast<plGameObject*>(pTargetObject);
  if (pGameObject->GetWorld() != plCSharpExecutionScope::GetCurrentWorld())
    return plCSharpStatus::InvalidArgument;

  plComponent* pSender = nullptr;
  if (routing == plCSharpMessageRouting::Event)
  {
    void* pSenderObject = nullptr;
    const plRTTI* pSenderType = nullptr;
    if (plCSharpObjectRegistry::GetSingleton().ResolveObject(
          senderComponent, pSenderObject, pSenderType) != plCSharpStatus::Success ||
        pSenderType == nullptr || !pSenderType->IsDerivedFrom<plComponent>())
    {
      return plCSharpStatus::InvalidArgument;
    }
    pSender = static_cast<plComponent*>(pSenderObject);
    if (pSender->GetWorld() != pGameObject->GetWorld())
      return plCSharpStatus::InvalidArgument;
  }

  plStringBuilder typeName(GetUtf8View(nativeTypeName));
  const plRTTI* pMessageType = plRTTI::FindTypeByName(typeName);
  if (pMessageType == nullptr || !pMessageType->IsDerivedFrom<plMessage>() ||
      pMessageType->GetAllocator() == nullptr ||
      !pMessageType->GetAllocator()->CanAllocate())
  {
    return plCSharpStatus::TypeNotFound;
  }

  plUniquePtr<plMessage> pMessage =
    pMessageType->GetAllocator()->Allocate<plMessage>();
  if (pMessage == nullptr ||
      PopulateMessageFromJson(*pMessage, GetUtf8View(payloadJson)).Failed())
  {
    return plCSharpStatus::InvalidValue;
  }

  switch (routing)
  {
    case plCSharpMessageRouting::Direct:
      pGameObject->SendMessage(*pMessage);
      break;
    case plCSharpMessageRouting::Recursive:
      pGameObject->SendMessageRecursive(*pMessage);
      break;
    case plCSharpMessageRouting::Event:
      pGameObject->SendEventMessage(*pMessage, pSender);
      break;
    default:
      return plCSharpStatus::InvalidArgument;
  }

  return plCSharpStatus::Success;
}

plCSharpStatus PL_CSHARP_CALL plCSharpHost::NativeSendManagedMessage(
  plCSharpObjectHandle target, plCSharpObjectHandle senderComponent,
  plUInt64 uiMessageId, plUInt64 uiPayloadToken,
  plCSharpMessageRouting routing)
{
  if (uiMessageId == 0 || uiPayloadToken == 0)
    return plCSharpStatus::InvalidArgument;

  void* pTargetObject = nullptr;
  const plRTTI* pTargetType = nullptr;
  if (plCSharpObjectRegistry::GetSingleton().ResolveObject(
        target, pTargetObject, pTargetType) != plCSharpStatus::Success ||
      pTargetType == nullptr || !pTargetType->IsDerivedFrom<plGameObject>())
  {
    return plCSharpStatus::InvalidArgument;
  }

  auto* pGameObject = static_cast<plGameObject*>(pTargetObject);
  if (pGameObject->GetWorld() != plCSharpExecutionScope::GetCurrentWorld())
    return plCSharpStatus::InvalidArgument;

  plComponent* pSender = nullptr;
  if (routing == plCSharpMessageRouting::Event)
  {
    void* pSenderObject = nullptr;
    const plRTTI* pSenderType = nullptr;
    if (plCSharpObjectRegistry::GetSingleton().ResolveObject(
          senderComponent, pSenderObject, pSenderType) != plCSharpStatus::Success ||
        pSenderType == nullptr || !pSenderType->IsDerivedFrom<plComponent>())
    {
      return plCSharpStatus::InvalidArgument;
    }

    pSender = static_cast<plComponent*>(pSenderObject);
    if (pSender->GetWorld() != pGameObject->GetWorld())
      return plCSharpStatus::InvalidArgument;
  }

  plMsgDeliverCSharpMsg message;
  message.m_uiMessageId = uiMessageId;
  message.m_uiPayloadToken = uiPayloadToken;

  switch (routing)
  {
    case plCSharpMessageRouting::Direct:
      pGameObject->SendMessage(message);
      break;
    case plCSharpMessageRouting::Recursive:
      pGameObject->SendMessageRecursive(message);
      break;
    case plCSharpMessageRouting::Event:
      pGameObject->SendEventMessage(message, pSender);
      break;
    default:
      return plCSharpStatus::InvalidArgument;
  }

  return plCSharpStatus::Success;
}

plCSharpStatus PL_CSHARP_CALL plCSharpHost::NativeM0Probe(plInt64 iInput, plInt64* out_pOutput)
{
  if (out_pOutput == nullptr)
  {
    return plCSharpStatus::InvalidArgument;
  }

  *out_pOutput = iInput + 1;
  return plCSharpStatus::Success;
}

PL_STATICLINK_FILE(CSharpPlugin, CSharpPlugin_Hosting_CSharpHost);
