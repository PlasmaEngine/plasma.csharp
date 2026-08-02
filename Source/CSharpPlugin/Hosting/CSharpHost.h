#pragma once

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <CSharpPlugin/Hosting/CSharpAbi.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Threading/Mutex.h>

/// Process-wide .NET host. CoreCLR is initialized once; collectible script generations live behind
/// the permanent managed bootstrap returned by GetManagedApi().
class PL_CSHARPPLUGIN_DLL plCSharpHost
{
  PL_DECLARE_SINGLETON(plCSharpHost);

public:
  plCSharpHost();
  ~plCSharpHost();

  plResult Initialize(plStringView sManagedRoot = {});
  void Shutdown();

  bool IsInitialized() const;
  const plCSharpManagedApiV1* GetManagedApi() const;

  /// Executes the M0 load/invoke/field/message/unload/stale-generation probe repeatedly.
  /// The managed game assembly is resolved below the application directory.
  plResult RunM0StressTest(plUInt32 uiCycles = 1000);

  void LogManagedError(plStringView sOperation, plCSharpStatus status) const;

private:
  enum class InitializationState : plUInt8
  {
    Uninitialized,
    Initialized,
    Failed,
    ShuttingDown,
  };

  plResult LoadRuntimeAndBootstrap(plStringView sManagedRoot);

  static plCSharpStatus PL_CSHARP_CALL NativeLog(plCSharpLogLevel level, plCSharpUtf8Span message);
  static plCSharpStatus PL_CSHARP_CALL NativeValidateObject(plCSharpObjectHandle object, plUInt32 uiExpectedKind);
  static plCSharpStatus PL_CSHARP_CALL NativeInvokeReflected(plUInt64 uiFunctionId, plCSharpObjectHandle target,
    const plCSharpValue* pArguments, plUInt32 uiArgumentCount, plCSharpValue* out_pResult);
  static plCSharpStatus PL_CSHARP_CALL NativeReleaseValue(plCSharpValue* pValue);
  static plCSharpStatus PL_CSHARP_CALL NativeQueryExtension(plCSharpUtf8Span name, plUInt32 uiMinimumVersion, void** out_ppApi);
  static plCSharpStatus PL_CSHARP_CALL NativeSendMessage(plCSharpObjectHandle target,
    plCSharpObjectHandle senderComponent, plCSharpUtf8Span nativeTypeName,
    plCSharpUtf8Span payloadJson, plCSharpMessageRouting routing);
  static plCSharpStatus PL_CSHARP_CALL NativeSendManagedMessage(
    plCSharpObjectHandle target, plCSharpObjectHandle senderComponent,
    plUInt64 uiMessageId, plUInt64 uiPayloadToken,
    plCSharpMessageRouting routing);
  static plCSharpStatus PL_CSHARP_CALL NativeM0Probe(plInt64 iInput, plInt64* out_pOutput);

  plCSharpNativeApiV1 m_NativeApi;
  plCSharpManagedApiV1 m_ManagedApi;
  void* m_pHostFxrModule = nullptr;
  mutable plMutex m_InitializationMutex;
  InitializationState m_InitializationState = InitializationState::Uninitialized;
};
