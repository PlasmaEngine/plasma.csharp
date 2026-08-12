#pragma once

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <CSharpPlugin/Hosting/CSharpAbi.h>
#include <Core/Console/ConsoleFunction.h>
#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Types/UniquePtr.h>
#include <Foundation/Types/Variant.h>
#include <GameEngine/Console/ConsoleTool.h>

/// Owns the strings a console function only borrows.
///
/// plConsoleFunctionBase keeps plStringViews, which is fine for the C++ functions it was written for
/// because their names are string literals in a translation unit. A command declared in a script has
/// a name that lives exactly as long as its generation, so the storage has to be owned - and it has
/// to be constructed before plConsoleFunctionBase's constructor sees the views. Listing this base
/// first is what guarantees that ordering.
struct plCSharpConsoleFunctionStorage
{
  plCSharpConsoleFunctionStorage(plStringView sName, plStringView sHelp)
    : m_sName(sName)
    , m_sHelp(sHelp)
  {
  }

  plString m_sName;
  plString m_sHelp;
};

/// One [ConsoleCommand] method, exposed to the engine console.
class PL_CSHARPPLUGIN_DLL plCSharpConsoleFunction : private plCSharpConsoleFunctionStorage, public plConsoleFunctionBase
{
public:
  plCSharpConsoleFunction(plStringView sName, plStringView sHelp, plUInt64 uiGeneration,
    plUInt64 uiCommandId, plDynamicArray<plVariantType::Enum>&& parameterTypes);

  virtual plUInt32 GetNumParameters() const override;
  virtual plVariant::Type::Enum GetParameterType(plUInt32 uiParam) const override;
  virtual plResult Call(plArrayPtr<plVariant> params) override;

private:
  plUInt64 m_uiGeneration = 0;
  plUInt64 m_uiCommandId = 0;
  plDynamicArray<plVariantType::Enum> m_ParameterTypes;
};

/// One [ConsoleTool] class, given a window by the engine console.
///
/// The console owns the Begin/End pair and the ImGui stack guard around OnDrawImGui, so this only
/// has to get the call into managed code and turn a failure into something visible rather than a
/// crash mid-frame.
class PL_CSHARPPLUGIN_DLL plCSharpConsoleTool : public plConsoleTool
{
public:
  plCSharpConsoleTool(plStringView sName, plStringView sCategory, bool bPinned,
    plUInt64 uiGeneration, plUInt64 uiToolId);

  virtual void OnDrawImGui() override;
  virtual bool IsPinned() const override { return m_bPinned; }

private:
  bool m_bPinned = false;
  plUInt64 m_uiGeneration = 0;
  plUInt64 m_uiToolId = 0;
  /// A tool that fails repeatedly is dropped rather than logging once per frame forever.
  plUInt8 m_uiFailureCount = 0;
};

/// Creates and destroys the console functions a script generation declares.
///
/// Registration is driven by generation lifetime rather than by the console: a command holds a method
/// id into a collectible load context, so it has to be gone before that context is unloaded.
class PL_CSHARPPLUGIN_DLL plCSharpConsoleRegistry
{
public:
  static plCSharpConsoleRegistry& GetSingleton();

  /// Parses the "commands" array out of a generation's descriptor manifest and registers each one.
  void RegisterGeneration(plUInt64 uiGeneration, plStringView sDescriptorJson);

  /// Removes every command of a generation. Safe to call for a generation that registered none, and
  /// must run even when the following unload fails.
  void UnregisterGeneration(plUInt64 uiGeneration);

  /// The console output table handed to managed code.
  static const plCSharpConsoleApiV1* GetConsoleApi();

private:
  struct GenerationEntry
  {
    plDynamicArray<plUniquePtr<plCSharpConsoleFunction>> m_Commands;
    plDynamicArray<plUniquePtr<plCSharpConsoleTool>> m_Tools;
  };

  mutable plMutex m_Mutex;
  plHashTable<plUInt64, GenerationEntry> m_Generations;
};
