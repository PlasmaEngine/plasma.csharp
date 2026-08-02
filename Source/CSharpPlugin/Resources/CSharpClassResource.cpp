#include <CSharpPlugin/CSharpPluginPCH.h>

#include <CSharpPlugin/Resources/CSharpClassResource.h>
#include <CSharpPlugin/Runtime/CSharpBindingRuntime.h>
#include <CSharpPlugin/Runtime/CSharpFunctionProperty.h>
#include <Core/Scripting/ScriptComponent.h>
#include <Foundation/Configuration/Startup.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Strings/PathUtils.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <Foundation/Utilities/ConversionUtils.h>

namespace
{
  constexpr plTypeVersion s_uiCSharpProjectVersion = 1;
  constexpr plUInt32 s_uiMaxProjectFiles = 4096;
  constexpr plUInt32 s_uiMaxScriptClasses = 16384;
  constexpr plUInt32 s_uiMaxFieldsPerClass = 4096;
  constexpr plUInt32 s_uiMaxFieldEntries = 256;
  constexpr plUInt32 s_uiMaxPathBytes = 32 * 1024;
  constexpr plUInt32 s_uiMaxIdentifierBytes = 4 * 1024;
  constexpr plUInt32 s_uiMaxDefaultValueBytes = 64 * 1024;
  constexpr plUInt64 s_uiMaxEmbeddedFileSize = 256ull * 1024ull * 1024ull;
  constexpr plUInt64 s_uiMaxEmbeddedProjectSize = 512ull * 1024ull * 1024ull;
  constexpr plUInt64 s_uiMaxContainerSize = 512ull * 1024ull * 1024ull;
  static plMutex s_CSharpProjectLoadMutex;
  static plAtomicBool s_bBindingSurfaceWarningLogged = false;

  static plResult ReadUInt16(plStreamReader& stream, plUInt16& out_uiValue)
  {
    return stream.ReadWordValue(&out_uiValue);
  }

  static plResult ReadUInt32(plStreamReader& stream, plUInt32& out_uiValue)
  {
    return stream.ReadDWordValue(&out_uiValue);
  }

  static plResult ReadUInt64(plStreamReader& stream, plUInt64& out_uiValue)
  {
    return stream.ReadQWordValue(&out_uiValue);
  }

  static plResult ReadUuid(plStreamReader& stream, plUuid& out_value)
  {
    // plUuid's stream format writes high then low.
    plUInt64 uiHigh = 0;
    plUInt64 uiLow = 0;
    PL_SUCCEED_OR_RETURN(ReadUInt64(stream, uiHigh));
    PL_SUCCEED_OR_RETURN(ReadUInt64(stream, uiLow));
    out_value = plUuid(uiLow, uiHigh);
    return PL_SUCCESS;
  }

  struct ResolvedResourceId
  {
    plString m_sContainerPath;
    plUuid m_SubAssetGuid;
  };

  static bool SplitResourceId(
    plStringView sResourceId, ResolvedResourceId& out_result)
  {
    const char* szSeparator = sResourceId.FindLastSubString("|");
    if (szSeparator == nullptr)
      return false;

    const plStringView sGuid(szSeparator + 1, sResourceId.GetEndPointer());
    if (!plConversionUtils::IsStringUuid(sGuid))
      return false;

    out_result.m_sContainerPath =
      plStringView(sResourceId.GetStartPointer(), szSeparator);
    out_result.m_SubAssetGuid = plConversionUtils::ConvertStringToUuid(sGuid);
    return !out_result.m_sContainerPath.IsEmpty();
  }

  static bool IsSafeRelativePath(plStringView sPath, plStringBuilder& out_sClean)
  {
    out_sClean = sPath;
    out_sClean.MakeCleanPath();
    return !out_sClean.IsEmpty() && plPathUtils::IsRelativePath(out_sClean) &&
           !plPathUtils::IsRootedPath(out_sClean) &&
           out_sClean != ".." && !out_sClean.StartsWith("../") &&
           !out_sClean.StartsWith("..\\");
  }

  static plResult ExtractEmbeddedFile(plStreamReader& stream,
    plStringView sExtractionRoot, plStringView sRelativePath, plUInt64 uiByteCount)
  {
    if (uiByteCount > s_uiMaxEmbeddedFileSize ||
        uiByteCount > plMath::MaxValue<plUInt32>())
    {
      return PL_FAILURE;
    }

    plStringBuilder sCleanRelative;
    if (!IsSafeRelativePath(sRelativePath, sCleanRelative))
      return PL_FAILURE;

    plStringBuilder sTarget = sExtractionRoot;
    sTarget.AppendPath(sCleanRelative);
    sTarget.MakeCleanPath();
    if (!plPathUtils::IsSubPath(sExtractionRoot, sTarget))
      return PL_FAILURE;

    if (plOSFile::CreateDirectoryStructure(
          plPathUtils::GetFileDirectory(sTarget))
          .Failed())
    {
      return PL_FAILURE;
    }

    plDynamicArray<plUInt8> bytes;
    bytes.SetCountUninitialized(static_cast<plUInt32>(uiByteCount));
    if (stream.ReadBytes(bytes.GetData(), uiByteCount) != uiByteCount)
      return PL_FAILURE;

    plOSFile file;
    if (file.Open(sTarget, plFileOpenMode::Write).Failed())
      return PL_FAILURE;

    return file.Write(bytes.GetData(), bytes.GetCount());
  }

  static const plVariant* GetValue(
    const plVariantDictionary& dictionary, plStringView sName)
  {
    return dictionary.GetValue(sName);
  }

  static bool ReadString(const plVariantDictionary& dictionary,
    plStringView sName, plString& out_sValue)
  {
    const plVariant* pValue = GetValue(dictionary, sName);
    if (pValue == nullptr || !pValue->IsA<plString>())
      return false;

    out_sValue = pValue->Get<plString>();
    return true;
  }

  static bool ReadHexId(const plVariantDictionary& dictionary,
    plStringView sName, plUInt64& out_uiValue)
  {
    plString text;
    return ReadString(dictionary, sName, text) &&
           plConversionUtils::ConvertHexStringToUInt64(text, out_uiValue).Succeeded();
  }

  static plResult BuildRuntimeDescriptors(plStringView sDescriptorJson,
    plUInt64 uiTypeId, plScriptRTTI::FunctionList& out_functions,
    plScriptRTTI::MessageHandlerList& out_messageHandlers)
  {
    out_functions.SetCount(plComponent_ScriptBaseClassFunctions::Count);

    plRawMemoryStreamReader stream(
      sDescriptorJson.GetStartPointer(), sDescriptorJson.GetElementCount());
    plJSONReader reader;
    if (reader.Parse(stream).Failed() ||
        reader.GetTopLevelElementType() != plJSONReader::ElementType::Dictionary)
    {
      plLog::Error("The managed C# generation descriptor is invalid JSON.");
      return PL_FAILURE;
    }

    const plVariant* pTypes = reader.GetTopLevelObject().GetValue("types");
    if (pTypes == nullptr || !pTypes->IsA<plVariantArray>())
      return PL_FAILURE;

    for (const plVariant& typeValue : pTypes->Get<plVariantArray>())
    {
      if (!typeValue.IsA<plVariantDictionary>())
        continue;

      const plVariantDictionary& type = typeValue.Get<plVariantDictionary>();
      plUInt64 uiDescriptorTypeId = 0;
      if (!ReadHexId(type, "id", uiDescriptorTypeId) ||
          uiDescriptorTypeId != uiTypeId)
      {
        continue;
      }

      if (const plVariant* pLifecycle = type.GetValue("lifecycle");
        pLifecycle != nullptr && pLifecycle->IsA<plVariantArray>())
      {
        for (const plVariant& methodValue : pLifecycle->Get<plVariantArray>())
        {
          if (!methodValue.IsA<plVariantDictionary>())
            continue;

          const auto& method = methodValue.Get<plVariantDictionary>();
          plUInt64 uiMethodId = 0;
          plString sName;
          if (!ReadHexId(method, "id", uiMethodId) ||
              !ReadString(method, "name", sName) ||
              uiMethodId >= plComponent_ScriptBaseClassFunctions::Count ||
              out_functions[static_cast<plUInt32>(uiMethodId)] != nullptr)
          {
            return PL_FAILURE;
          }

          out_functions[static_cast<plUInt32>(uiMethodId)] =
            PL_SCRIPT_NEW(plCSharpFunctionProperty, sName, uiMethodId);
        }
      }

      if (const plVariant* pMessages = type.GetValue("messages");
        pMessages != nullptr && pMessages->IsA<plVariantArray>())
      {
        bool bHasCustomMessageEnvelope = false;
        for (const plVariant& messageValue : pMessages->Get<plVariantArray>())
        {
          if (!messageValue.IsA<plVariantDictionary>())
            continue;

          const auto& message = messageValue.Get<plVariantDictionary>();
          plUInt64 uiMessageId = 0;
          plString sNativeTypeName;
          if (!ReadHexId(message, "id", uiMessageId) ||
              !ReadString(message, "nativeTypeName", sNativeTypeName))
          {
            return PL_FAILURE;
          }

          if (sNativeTypeName == "plMsgDeliverCSharpMsg")
          {
            if (bHasCustomMessageEnvelope)
              continue;

            bHasCustomMessageEnvelope = true;
            uiMessageId = 0; // The envelope supplies the custom message type ID.
          }

          const plRTTI* pMessageType = plRTTI::FindTypeByName(sNativeTypeName);
          if (pMessageType == nullptr || !pMessageType->IsDerivedFrom<plMessage>() ||
              pMessageType->GetAllocator() == nullptr)
          {
            plLog::Error(
              "C# message handler references unavailable native message type '{}'.",
              sNativeTypeName);
            return PL_FAILURE;
          }

          plDynamicArray<const plAbstractProperty*> properties;
          pMessageType->GetAllProperties(properties);
          plScriptMessageDesc desc;
          desc.m_pType = pMessageType;
          desc.m_Properties = properties;
          out_messageHandlers.PushBack(
            PL_SCRIPT_NEW(plCSharpMessageHandler, desc, uiMessageId));
        }
      }

      return PL_SUCCESS;
    }

    plLog::Error(
      "C# type ID '{}' was not present in the managed generation descriptor.",
      plArgU(uiTypeId, 16, true, 16, true));
    return PL_FAILURE;
  }

  static bool IsCSharpContainerResource(plStringView sResourceId)
  {
    const char* szSeparator = sResourceId.FindLastSubString("|");
    const plStringView sPath = szSeparator == nullptr
                                 ? sResourceId
                                 : plStringView(sResourceId.GetStartPointer(), szSeparator);
    return plPathUtils::HasExtension(sPath, ".plBinCSharpProject");
  }
} // namespace

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plCSharpClassResource, 1, plRTTIDefaultAllocator<plCSharpClassResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;
PL_RESOURCE_IMPLEMENT_COMMON_CODE(plCSharpClassResource);

static plCSharpClassResourceLoader s_CSharpClassResourceLoader;

PL_BEGIN_SUBSYSTEM_DECLARATION(CSharpPlugin, CSharpClassResource)

  BEGIN_SUBSYSTEM_DEPENDENCIES
    "ResourceManager",
    "CSharpHost"
  END_SUBSYSTEM_DEPENDENCIES

  ON_CORESYSTEMS_STARTUP
  {
    const plRTTI* pResourceType = plGetStaticRTTI<plCSharpClassResource>();

    plResourceManager::RegisterResourceForAssetType("C# Script Class", pResourceType);
    plResourceManager::RegisterResourceForAssetType("CSharpScriptClass", pResourceType);
    plResourceManager::RegisterResourceOverrideType(
      pResourceType,
      [](const plStringBuilder& sResourceId) -> bool
      {
        return IsCSharpContainerResource(sResourceId);
      });
  }

  ON_HIGHLEVELSYSTEMS_STARTUP
  {
    plResourceManager::SetResourceTypeLoader<plCSharpClassResource>(
      &s_CSharpClassResourceLoader);
  }

  ON_HIGHLEVELSYSTEMS_SHUTDOWN
  {
    plResourceManager::SetResourceTypeLoader<plCSharpClassResource>(nullptr);
    plCSharpClassResource::CleanupDynamicPluginReferences();
  }

  ON_CORESYSTEMS_SHUTDOWN
  {
    plResourceManager::UnregisterResourceOverrideType(
      plGetStaticRTTI<plCSharpClassResource>());
  }

PL_END_SUBSYSTEM_DECLARATION;
// clang-format on

plCSharpClassResource::plCSharpClassResource() = default;
plCSharpClassResource::~plCSharpClassResource() = default;

plResourceLoadDesc plCSharpClassResource::UnloadData(Unload WhatToUnload)
{
  PL_IGNORE_UNUSED(WhatToUnload);

  DeleteScriptType();
  m_pClassData = nullptr;

  plResourceLoadDesc result;
  result.m_State = plResourceState::Unloaded;
  result.m_uiQualityLevelsDiscardable = 0;
  result.m_uiQualityLevelsLoadable = 0;
  return result;
}

plResourceLoadDesc plCSharpClassResource::UpdateContent(plStreamReader* pStream)
{
  plResourceLoadDesc result;
  result.m_State = plResourceState::LoadedResourceMissing;
  result.m_uiQualityLevelsDiscardable = 0;
  result.m_uiQualityLevelsLoadable = 0;

  if (pStream == nullptr)
    return result;

  plString sAbsoluteContainerPath;
  plUuid requestedSubAsset;
  if (pStream->ReadString(sAbsoluteContainerPath, s_uiMaxPathBytes).Failed() ||
      ReadUuid(*pStream, requestedSubAsset).Failed())
  {
    return result;
  }
  if (!requestedSubAsset.IsValid())
  {
    plLog::Error("A C# project container was loaded without a script-class selector.");
    return result;
  }

  plAssetFileHeader assetHeader;
  if (assetHeader.Read(*pStream).Failed())
    return result;

  plUInt16 uiContainerVersion = 0;
  if (ReadUInt16(*pStream, uiContainerVersion).Failed() ||
      uiContainerVersion != s_uiCSharpProjectVersion)
  {
    plLog::Error("Unsupported C# project container version.");
    return result;
  }

  plString sEntryAssembly;
  plUInt64 uiBindingSchemaHash = 0;
  if (pStream->ReadString(sEntryAssembly, s_uiMaxPathBytes).Failed() ||
      ReadUInt64(*pStream, uiBindingSchemaHash).Failed())
  {
    return result;
  }

  plUInt64 uiRuntimeSchemaHash = 0;
  if (plCSharpBindingRuntime::GetSingleton().GetSchemaHash(uiRuntimeSchemaHash).Failed())
    return result;

  if (uiRuntimeSchemaHash != uiBindingSchemaHash)
  {
    // The manifest is generated in the editor, while the resource is consumed by
    // EditorEngineProcess or Player. Those processes can legitimately expose a
    // different set of plugin-owned reflected types even when every binding used by
    // this assembly is compatible. Treat the whole-surface hash as a diagnostic only.
    // The native/managed ABI table is validated strictly by plCSharpHost, and each
    // reflected call is resolved through its stable ID with argument type checking.
    if (s_bBindingSurfaceWarningLogged.TestAndSet(false, true))
    {
      plLog::Warning(
        "C# project binding surface differs from the runtime. Project: {}, runtime: {}. "
        "This does not prevent C# scripts from running; calls to bindings unavailable in this runtime will fail at the call site.",
        plArgU(uiBindingSchemaHash, 16, true, 16, true),
        plArgU(uiRuntimeSchemaHash, 16, true, 16, true));
    }
  }

  // Different class subassets share the same extracted project files. Serialize extraction
  // through the generation's shadow copy so no loader can observe a partially rewritten DLL.
  PL_LOCK(s_CSharpProjectLoadMutex);

  plStringBuilder sHashFolder;
  sHashFolder.SetFormat(
    "{0}", plArgU(assetHeader.GetFileHash(), 16, true, 16, true));
  plStringBuilder sExtractionRoot =
    plOSFile::GetTempDataFolder("PlasmaEngine/CSharp/Projects");
  sExtractionRoot.AppendPath(sHashFolder);
  sExtractionRoot.MakeCleanPath();
  if (plOSFile::CreateDirectoryStructure(sExtractionRoot).Failed())
    return result;

  plUInt32 uiFileCount = 0;
  if (ReadUInt32(*pStream, uiFileCount).Failed())
    return result;
  if (uiFileCount == 0 || uiFileCount > s_uiMaxProjectFiles)
    return result;

  plUInt64 uiEmbeddedProjectSize = 0;
  for (plUInt32 i = 0; i < uiFileCount; ++i)
  {
    plString sRelativePath;
    plUInt64 uiByteCount = 0;
    if (pStream->ReadString(sRelativePath, s_uiMaxPathBytes).Failed() ||
        ReadUInt64(*pStream, uiByteCount).Failed())
    {
      return result;
    }
    if (uiByteCount > s_uiMaxEmbeddedProjectSize - uiEmbeddedProjectSize)
      return result;
    uiEmbeddedProjectSize += uiByteCount;

    if (ExtractEmbeddedFile(
          *pStream, sExtractionRoot, sRelativePath, uiByteCount)
          .Failed())
    {
      plLog::Error(
        "Failed to extract embedded C# project file '{}'.", sRelativePath);
      return result;
    }
  }

  plStringBuilder sCleanEntryAssembly;
  if (!IsSafeRelativePath(sEntryAssembly, sCleanEntryAssembly))
    return result;

  plStringBuilder sAssemblyPath = sExtractionRoot;
  sAssemblyPath.AppendPath(sCleanEntryAssembly);
  sAssemblyPath.MakeCleanPath();
  if (!plPathUtils::IsSubPath(sExtractionRoot, sAssemblyPath) ||
      !plOSFile::ExistsFile(sAssemblyPath))
  {
    plLog::Error(
      "The C# project entry assembly '{}' is missing from the container.",
      sEntryAssembly);
    return result;
  }

  plSharedPtr<plCSharpClassData> pClassData =
    PL_DEFAULT_NEW(plCSharpClassData);
  plUuid persistentGuid;
  bool bFoundClass = false;

  plUInt32 uiClassCount = 0;
  if (ReadUInt32(*pStream, uiClassCount).Failed())
    return result;
  if (uiClassCount == 0 || uiClassCount > s_uiMaxScriptClasses)
    return result;

  for (plUInt32 classIndex = 0; classIndex < uiClassCount; ++classIndex)
  {
    plUuid subAssetGuid;
    plUuid classPersistentGuid;
    plString sTypeIdHex;
    plString sManagedName;
    plUInt32 uiFieldCount = 0;
    if (ReadUuid(*pStream, subAssetGuid).Failed() ||
        ReadUuid(*pStream, classPersistentGuid).Failed() ||
        pStream->ReadString(sTypeIdHex, 32).Failed() ||
        pStream->ReadString(sManagedName, s_uiMaxIdentifierBytes).Failed() ||
        ReadUInt32(*pStream, uiFieldCount).Failed())
    {
      return result;
    }
    if (uiFieldCount > s_uiMaxFieldsPerClass)
      return result;

    const bool bSelected = subAssetGuid == requestedSubAsset;
    if (bSelected)
    {
      if (bFoundClass ||
          plConversionUtils::ConvertHexStringToUInt64(
            sTypeIdHex, pClassData->m_uiTypeId)
            .Failed())
      {
        return result;
      }

      pClassData->m_sManagedName = sManagedName;
      persistentGuid = classPersistentGuid;
      bFoundClass = true;
      pClassData->m_Fields.Reserve(uiFieldCount);
    }

    for (plUInt32 fieldIndex = 0; fieldIndex < uiFieldCount; ++fieldIndex)
    {
      plString sFieldIdHex;
      plString sFieldName;
      plString sManagedType;
      plString sDefaultKind;
      plString sDefaultValue;
      if (pStream->ReadString(sFieldIdHex, 32).Failed() ||
          pStream->ReadString(sFieldName, s_uiMaxIdentifierBytes).Failed() ||
          pStream->ReadString(sManagedType, s_uiMaxIdentifierBytes).Failed() ||
          pStream->ReadString(sDefaultKind, 64).Failed() ||
          pStream->ReadString(sDefaultValue, s_uiMaxDefaultValueBytes).Failed())
      {
        return result;
      }

      plUInt32 uiComponentCount = 0;
      if (ReadUInt32(*pStream, uiComponentCount).Failed())
        return result;
      if (uiComponentCount > s_uiMaxFieldEntries)
        return result;
      for (plUInt32 componentIndex = 0;
        componentIndex < uiComponentCount; ++componentIndex)
      {
        plString sKey;
        plString sValue;
        if (pStream->ReadString(sKey, s_uiMaxIdentifierBytes).Failed() ||
            pStream->ReadString(sValue, s_uiMaxDefaultValueBytes).Failed())
        {
          return result;
        }
      }

      plUInt32 uiMetadataCount = 0;
      if (ReadUInt32(*pStream, uiMetadataCount).Failed())
        return result;
      if (uiMetadataCount > s_uiMaxFieldEntries)
        return result;
      for (plUInt32 metadataIndex = 0;
        metadataIndex < uiMetadataCount; ++metadataIndex)
      {
        plString sKey;
        plString sValue;
        if (pStream->ReadString(sKey, s_uiMaxIdentifierBytes).Failed() ||
            pStream->ReadString(sValue, s_uiMaxDefaultValueBytes).Failed())
        {
          return result;
        }
      }

      if (bSelected)
      {
        plCSharpFieldInfo& field = pClassData->m_Fields.ExpandAndGetRef();
        if (plConversionUtils::ConvertHexStringToUInt64(
              sFieldIdHex, field.m_uiId)
              .Failed())
        {
          return result;
        }
        field.m_sName.Assign(sFieldName);
        field.m_sManagedType = sManagedType;
      }
    }
  }

  if (!bFoundClass || !persistentGuid.IsValid())
  {
    plLog::Error(
      "C# script-class subasset '{}' was not found in '{}'.",
      requestedSubAsset, sAbsoluteContainerPath);
    return result;
  }

  // The managed loader recursively copies the assembly directory. Keep its destination
  // outside that source tree, otherwise the copy would discover its own output.
  plStringBuilder sShadowCopyRoot =
    plOSFile::GetTempDataFolder("PlasmaEngine/CSharp/ShadowCopies");
  sShadowCopyRoot.AppendPath(sHashFolder);

  plCSharpGenerationLease lease;
  plString sDescriptorJson;
  if (plCSharpProjectRuntime::GetSingleton()
        .AcquireGeneration(
          sAssemblyPath, sShadowCopyRoot, lease, sDescriptorJson)
        .Failed())
  {
    return result;
  }

  plSharedPtr<plCSharpGenerationContext> pGeneration =
    PL_DEFAULT_NEW(plCSharpGenerationContext, std::move(lease));
  pClassData->m_pGeneration = pGeneration;

  plScriptRTTI::FunctionList functions;
  plScriptRTTI::MessageHandlerList messageHandlers;
  if (BuildRuntimeDescriptors(sDescriptorJson, pClassData->m_uiTypeId,
        functions, messageHandlers)
        .Failed())
  {
    return result;
  }

  plStringBuilder sRuntimeTypeName;
  sRuntimeTypeName.SetFormat("{}@{}",
    pClassData->m_sManagedName,
    pGeneration->GetGeneration());

  DeleteScriptType();
  m_pClassData = pClassData;
  // Lifecycle entry points are declared on plComponent. CreateScriptType uses the
  // supplied base type's reflected functions to map the sparse backend function
  // list into plComponent_ScriptBaseClassFunctions slots, so passing
  // plScriptComponent here would silently discard Initialize,
  // OnSimulationStarted, Update, and the other lifecycle methods.
  CreateScriptType(sRuntimeTypeName, plGetStaticRTTI<plComponent>(),
    std::move(functions), std::move(messageHandlers));

  result.m_State = plResourceState::Loaded;
  return result;
}

void plCSharpClassResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plCSharpClassResource);
  if (m_pClassData != nullptr)
  {
    out_NewMemoryUsage.m_uiMemoryCPU +=
      m_pClassData->m_Fields.GetHeapMemoryUsage() +
      m_pClassData->m_sManagedName.GetHeapMemoryUsage();
  }
  out_NewMemoryUsage.m_uiMemoryGPU = 0;
}

plUniquePtr<plScriptInstance> plCSharpClassResource::Instantiate(
  plReflectedClass& inout_owner, plWorld* pWorld) const
{
  if (m_pClassData == nullptr)
    return nullptr;

  plUniquePtr<plCSharpInstance> pInstance =
    PL_SCRIPT_NEW(plCSharpInstance, inout_owner, pWorld, m_pClassData);
  if (!pInstance->IsValid())
    return nullptr;

  return std::move(pInstance);
}

plResourceLoadData plCSharpClassResourceLoader::OpenDataStream(
  const plResource* pResource)
{
  plResourceLoadData result;
  plStringBuilder sResolvedId;
  plFileSystem::ResolveAssetRedirection(
    pResource->GetResourceID(), sResolvedId);

  ResolvedResourceId resourceId;
  if (!SplitResourceId(sResolvedId, resourceId))
  {
    plLog::Error(
      "C# script-class resource ID is invalid: '{}'.", sResolvedId);
    return result;
  }

  plFileReader file;
  if (file.Open(resourceId.m_sContainerPath).Failed())
    return result;

  result.m_sResourceDescription = sResolvedId;
#if PL_ENABLED(PL_SUPPORTS_FILE_STATS)
  plFileStats stat;
  if (plFileSystem::GetFileStats(resourceId.m_sContainerPath, stat).Succeeded())
    result.m_LoadedFileModificationDate = stat.m_LastModificationTime;
#endif

  LoadedData* pData = PL_DEFAULT_NEW(LoadedData);
  const plUInt64 uiFileSize = file.GetFileSize();
  const plUInt64 uiPathBytes = file.GetFilePathAbsolute().GetElementCount();
  const plUInt64 uiHeaderCapacity = uiPathBytes + 64;
  if (uiPathBytes > s_uiMaxPathBytes ||
      uiHeaderCapacity > s_uiMaxContainerSize ||
      uiFileSize > s_uiMaxContainerSize - uiHeaderCapacity)
  {
    plLog::Error(
      "C# project container '{}' exceeds the {} MiB load limit.",
      resourceId.m_sContainerPath, s_uiMaxContainerSize / (1024 * 1024));
    PL_DEFAULT_DELETE(pData);
    return result;
  }
  const plUInt64 uiCapacity = uiFileSize + uiHeaderCapacity;
  pData->m_Storage.SetCountUninitialized(uiCapacity);

  plUInt8* pBytes = pData->m_Storage.GetBlobPtr<plUInt8>().GetPtr();
  plRawMemoryStreamWriter writer(pBytes, uiCapacity);
  writer << file.GetFilePathAbsolute();
  writer << resourceId.m_SubAssetGuid;
  const plUInt64 uiOffset = writer.GetNumWrittenBytes();
  if (file.ReadBytes(pBytes + uiOffset, uiFileSize) != uiFileSize)
  {
    PL_DEFAULT_DELETE(pData);
    return result;
  }

  pData->m_Reader.Reset(pBytes, uiOffset + uiFileSize);
  result.m_pDataStream = &pData->m_Reader;
  result.m_pCustomLoaderData = pData;
  return result;
}

void plCSharpClassResourceLoader::CloseDataStream(
  const plResource* pResource, const plResourceLoadData& loaderData)
{
  PL_IGNORE_UNUSED(pResource);
  LoadedData* pData =
    static_cast<LoadedData*>(loaderData.m_pCustomLoaderData);
  PL_DEFAULT_DELETE(pData);
}

bool plCSharpClassResourceLoader::IsResourceOutdated(
  const plResource* pResource) const
{
  plStringBuilder sResolvedId;
  plFileSystem::ResolveAssetRedirection(
    pResource->GetResourceID(), sResolvedId);

  ResolvedResourceId resourceId;
  if (!SplitResourceId(sResolvedId, resourceId))
    return false;

#if PL_ENABLED(PL_SUPPORTS_FILE_STATS)
  if (pResource->GetLoadedFileModificationTime().IsValid())
  {
    plFileStats stat;
    if (plFileSystem::GetFileStats(resourceId.m_sContainerPath, stat).Failed())
      return false;

    return !stat.m_LastModificationTime.Compare(
      pResource->GetLoadedFileModificationTime(),
      plTimestamp::CompareMode::FileTimeEqual);
  }
#endif

  return true;
}

PL_STATICLINK_FILE(
  CSharpPlugin, CSharpPlugin_Resources_CSharpClassResource);
