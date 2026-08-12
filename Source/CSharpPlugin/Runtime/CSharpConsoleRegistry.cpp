#include <CSharpPlugin/CSharpPluginPCH.h>

#include <CSharpPlugin/Hosting/CSharpHost.h>
#include <CSharpPlugin/Runtime/CSharpConsoleRegistry.h>
#include <CSharpPlugin/Runtime/CSharpProjectRuntime.h>

#include <Core/Console/Console.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Utilities/ConversionUtils.h>

namespace
{
  /// Mirrors Plasma.ScriptCommandParameterKind. The set is deliberately small: the console's command
  /// interpreter is Lua and only marshals these through plVariant.
  plVariantType::Enum ToVariantType(plUInt64 uiKind)
  {
    switch (uiKind)
    {
      case 0:
        return plVariantType::Bool;
      case 1:
        return plVariantType::Int32;
      case 2:
        return plVariantType::UInt32;
      case 3:
        return plVariantType::Float;
      case 4:
        return plVariantType::Double;
      case 5:
        return plVariantType::String;
      default:
        return plVariantType::Invalid;
    }
  }

  bool ReadHexId(const plVariantDictionary& dictionary, const char* szKey, plUInt64& out_uiValue)
  {
    const plVariant* pValue = dictionary.GetValue(szKey);
    if (pValue == nullptr || !pValue->IsA<plString>())
      return false;

    return plConversionUtils::ConvertHexStringToUInt64(pValue->Get<plString>(), out_uiValue).Succeeded();
  }

  bool ReadString(const plVariantDictionary& dictionary, const char* szKey, plString& out_sValue)
  {
    const plVariant* pValue = dictionary.GetValue(szKey);
    if (pValue == nullptr || !pValue->IsA<plString>())
      return false;

    out_sValue = pValue->Get<plString>();
    return true;
  }

  plCSharpStatus PL_CSHARP_CALL ConsolePrint(plUInt32 uiLineType, plCSharpUtf8Span text)
  {
    plConsole* pConsole = plConsole::GetMainConsole();
    if (pConsole == nullptr)
      return plCSharpStatus::NotInitialized;

    if (text.m_pData == nullptr || text.m_uiLength == 0)
      return plCSharpStatus::Success;

    const plConsoleString::Type type = (uiLineType <= static_cast<plUInt32>(plConsoleString::Type::Debug))
                                         ? static_cast<plConsoleString::Type>(uiLineType)
                                         : plConsoleString::Type::Default;

    pConsole->AddConsoleString(plStringView(text.m_pData, text.m_uiLength), type);
    return plCSharpStatus::Success;
  }

  const plCSharpManagedConsoleApiV1* GetManagedConsoleApi()
  {
    const plCSharpManagedApiV1* pManaged = plCSharpProjectRuntime::GetSingleton().GetManagedApi();
    if (pManaged == nullptr || pManaged->m_QueryExtension == nullptr)
      return nullptr;

    plCSharpUtf8Span name;
    const plStringView sName = "Plasma.Console";
    name.m_pData = sName.GetStartPointer();
    name.m_uiLength = sName.GetElementCount();

    void* pApi = nullptr;
    if (pManaged->m_QueryExtension(name, 1, &pApi) != plCSharpStatus::Success || pApi == nullptr)
      return nullptr;

    const auto* pConsoleApi = static_cast<const plCSharpManagedConsoleApiV1*>(pApi);
    if (pConsoleApi->m_uiSize < sizeof(plCSharpManagedConsoleApiV1) || pConsoleApi->m_InvokeCommand == nullptr)
      return nullptr;

    return pConsoleApi;
  }
} // namespace

plCSharpConsoleFunction::plCSharpConsoleFunction(plStringView sName, plStringView sHelp,
  plUInt64 uiGeneration, plUInt64 uiCommandId, plDynamicArray<plVariantType::Enum>&& parameterTypes)
  : plCSharpConsoleFunctionStorage(sName, sHelp)
  , plConsoleFunctionBase(m_sName, m_sHelp)
  , m_uiGeneration(uiGeneration)
  , m_uiCommandId(uiCommandId)
  , m_ParameterTypes(std::move(parameterTypes))
{
}

plUInt32 plCSharpConsoleFunction::GetNumParameters() const
{
  return m_ParameterTypes.GetCount();
}

plVariant::Type::Enum plCSharpConsoleFunction::GetParameterType(plUInt32 uiParam) const
{
  return uiParam < m_ParameterTypes.GetCount() ? m_ParameterTypes[uiParam] : plVariantType::Invalid;
}

plResult plCSharpConsoleFunction::Call(plArrayPtr<plVariant> params)
{
  if (params.GetCount() != m_ParameterTypes.GetCount())
    return PL_FAILURE;

  const plCSharpManagedConsoleApiV1* pApi = GetManagedConsoleApi();
  if (pApi == nullptr)
  {
    plLog::Error("C# console command '{}' cannot run: the managed console API is unavailable.", m_sName);
    return PL_FAILURE;
  }

  // Strings are passed as borrowed spans into the plVariant's own storage, so the converted copies
  // have to outlive the call. params is owned by the interpreter for the duration of Call().
  plHybridArray<plString, 6> stringStorage;
  plHybridArray<plCSharpValue, 6> arguments;
  arguments.SetCount(params.GetCount());

  for (plUInt32 i = 0; i < params.GetCount(); ++i)
  {
    plVariant converted = params[i];
    if (converted.CanConvertTo(m_ParameterTypes[i]))
      converted = converted.ConvertTo(m_ParameterTypes[i]);

    plCSharpValue& value = arguments[i];
    switch (m_ParameterTypes[i])
    {
      case plVariantType::Bool:
        value.m_Kind = plCSharpValueKind::Boolean;
        value.m_uiPayload0 = converted.ConvertTo<bool>() ? 1 : 0;
        break;

      case plVariantType::Int32:
        value.m_Kind = plCSharpValueKind::Int64;
        value.m_uiPayload0 = static_cast<plUInt64>(static_cast<plInt64>(converted.ConvertTo<plInt32>()));
        break;

      case plVariantType::UInt32:
        value.m_Kind = plCSharpValueKind::UInt64;
        value.m_uiPayload0 = converted.ConvertTo<plUInt32>();
        break;

      case plVariantType::Float:
      case plVariantType::Double:
      {
        const double dValue = converted.ConvertTo<double>();
        value.m_Kind = plCSharpValueKind::Double;
        plMemoryUtils::RawByteCopy(&value.m_uiPayload0, &dValue, sizeof(double));
        break;
      }

      case plVariantType::String:
      {
        plString& stored = stringStorage.ExpandAndGetRef();
        stored = converted.ConvertTo<plString>();
        value.m_Kind = plCSharpValueKind::Utf8String;
        value.m_uiPayload0 = reinterpret_cast<plUInt64>(stored.GetData());
        value.m_uiPayload1 = stored.GetElementCount();
        break;
      }

      default:
        return PL_FAILURE;
    }
  }

  const plCSharpStatus status =
    pApi->m_InvokeCommand(m_uiGeneration, m_uiCommandId, arguments.GetData(), arguments.GetCount());

  if (status != plCSharpStatus::Success)
  {
    if (plCSharpHost* pHost = plCSharpHost::GetSingleton())
      pHost->LogManagedError("invoke C# console command", status);

    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

plCSharpConsoleTool::plCSharpConsoleTool(plStringView sName, plStringView sCategory, bool bPinned,
  plUInt64 uiGeneration, plUInt64 uiToolId)
  : plConsoleTool(sName, sCategory)
  , m_bPinned(bPinned)
  , m_uiGeneration(uiGeneration)
  , m_uiToolId(uiToolId)
{
}

void plCSharpConsoleTool::OnDrawImGui()
{
  if (m_uiFailureCount >= 3)
    return;

  const plCSharpManagedConsoleApiV1* pApi = GetManagedConsoleApi();
  if (pApi == nullptr || pApi->m_DrawTool == nullptr)
  {
    ++m_uiFailureCount;
    return;
  }

  const plCSharpStatus status = pApi->m_DrawTool(m_uiGeneration, m_uiToolId);
  if (status == plCSharpStatus::Success)
  {
    m_uiFailureCount = 0;
    return;
  }

  ++m_uiFailureCount;
  if (plCSharpHost* pHost = plCSharpHost::GetSingleton())
    pHost->LogManagedError("draw C# console tool", status);

  if (m_uiFailureCount >= 3)
  {
    plLog::Error("C# console tool '{}' failed three times and will not be drawn again. "
                 "Rebuild the scripts to re-enable it.",
      GetName());
  }
}

plCSharpConsoleRegistry& plCSharpConsoleRegistry::GetSingleton()
{
  static plCSharpConsoleRegistry registry;
  return registry;
}

const plCSharpConsoleApiV1* plCSharpConsoleRegistry::GetConsoleApi()
{
  static const plCSharpConsoleApiV1 s_Api = []()
  {
    plCSharpConsoleApiV1 api;
    api.m_Print = &ConsolePrint;
    return api;
  }();

  return &s_Api;
}

void plCSharpConsoleRegistry::RegisterGeneration(plUInt64 uiGeneration, plStringView sDescriptorJson)
{
  plRawMemoryStreamReader stream(sDescriptorJson.GetStartPointer(), sDescriptorJson.GetElementCount());
  plJSONReader reader;
  if (reader.Parse(stream).Failed() ||
      reader.GetTopLevelElementType() != plJSONReader::ElementType::Dictionary)
  {
    return;
  }

  const plVariant* pCommands = reader.GetTopLevelObject().GetValue("commands");
  if (pCommands == nullptr || !pCommands->IsA<plVariantArray>())
    return;

  plDynamicArray<plUniquePtr<plCSharpConsoleFunction>> registered;

  for (const plVariant& commandValue : pCommands->Get<plVariantArray>())
  {
    if (!commandValue.IsA<plVariantDictionary>())
      continue;

    const plVariantDictionary& command = commandValue.Get<plVariantDictionary>();

    plUInt64 uiCommandId = 0;
    plString sName;
    plString sHelp;
    if (!ReadHexId(command, "id", uiCommandId) || !ReadString(command, "name", sName) || sName.IsEmpty())
      continue;

    ReadString(command, "help", sHelp);

    plDynamicArray<plVariantType::Enum> parameterTypes;
    bool bParametersValid = true;
    if (const plVariant* pParameters = command.GetValue("parameters");
        pParameters != nullptr && pParameters->IsA<plVariantArray>())
    {
      for (const plVariant& parameterValue : pParameters->Get<plVariantArray>())
      {
        if (!parameterValue.CanConvertTo<plUInt64>())
        {
          bParametersValid = false;
          break;
        }

        const plVariantType::Enum type = ToVariantType(parameterValue.ConvertTo<plUInt64>());
        if (type == plVariantType::Invalid)
        {
          bParametersValid = false;
          break;
        }

        parameterTypes.PushBack(type);
      }
    }

    if (!bParametersValid)
    {
      plLog::Error("C# console command '{}' declares an unsupported parameter type and was skipped.", sName);
      continue;
    }

    registered.PushBack(PL_DEFAULT_NEW(plCSharpConsoleFunction, sName, sHelp, uiGeneration, uiCommandId,
      std::move(parameterTypes)));
  }

  plDynamicArray<plUniquePtr<plCSharpConsoleTool>> registeredTools;

  if (const plVariant* pTools = reader.GetTopLevelObject().GetValue("tools");
      pTools != nullptr && pTools->IsA<plVariantArray>())
  {
    for (const plVariant& toolValue : pTools->Get<plVariantArray>())
    {
      if (!toolValue.IsA<plVariantDictionary>())
        continue;

      const plVariantDictionary& tool = toolValue.Get<plVariantDictionary>();

      plUInt64 uiToolId = 0;
      plString sName;
      plString sCategory;
      if (!ReadHexId(tool, "id", uiToolId) || !ReadString(tool, "name", sName) || sName.IsEmpty())
        continue;

      ReadString(tool, "category", sCategory);

      bool bPinned = false;
      if (const plVariant* pPinned = tool.GetValue("pinned"); pPinned != nullptr && pPinned->CanConvertTo<bool>())
        bPinned = pPinned->ConvertTo<bool>();

      registeredTools.PushBack(
        PL_DEFAULT_NEW(plCSharpConsoleTool, sName, sCategory, bPinned, uiGeneration, uiToolId));
    }
  }

  if (registered.IsEmpty() && registeredTools.IsEmpty())
    return;

  const plUInt32 uiCommandCount = registered.GetCount();
  const plUInt32 uiToolCount = registeredTools.GetCount();
  {
    PL_LOCK(m_Mutex);
    GenerationEntry& entry = m_Generations[uiGeneration];
    entry.m_Commands = std::move(registered);
    entry.m_Tools = std::move(registeredTools);
  }

  plLog::Success("Registered {} C# console command(s) and {} console tool(s) from generation {}.",
    uiCommandCount, uiToolCount, uiGeneration);
}

void plCSharpConsoleRegistry::UnregisterGeneration(plUInt64 uiGeneration)
{
  GenerationEntry removed;

  {
    PL_LOCK(m_Mutex);
    GenerationEntry* pEntry = nullptr;
    if (!m_Generations.TryGetValue(uiGeneration, pEntry) || pEntry == nullptr)
      return;

    removed.m_Commands = std::move(pEntry->m_Commands);
    removed.m_Tools = std::move(pEntry->m_Tools);
    m_Generations.Remove(uiGeneration);
  }

  // Destroyed outside the lock, but still before the caller unloads the load context - these objects
  // are what keep the console pointing into a generation that is about to disappear. A tool is worse
  // than a command here: the console would call it on the very next frame.
  removed.m_Tools.Clear();
  removed.m_Commands.Clear();
}
