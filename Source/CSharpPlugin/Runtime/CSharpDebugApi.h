#pragma once

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <CSharpPlugin/Hosting/CSharpAbi.h>

/// Supplies the "Plasma.Debug" extension table returned by plCSharpHost's QueryExtension.
///
/// Every entry draws into the world of the calling script's execution scope, so a debug call made
/// outside a script call fails with plCSharpStatus::NotInitialized rather than drawing nowhere.
class PL_CSHARPPLUGIN_DLL plCSharpDebugApi
{
public:
  static const plCSharpDebugApiV1* GetApi();
};
