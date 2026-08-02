#pragma once

#include <EditorPluginCSharp/EditorPluginCSharpDLL.h>
#include <Foundation/Strings/StringBuilder.h>

/// \brief Where this plugin's C# payload lives.
///
/// The payload - managed assemblies, tools, and the optional private .NET runtime - ships beside
/// the plugin, and where that is depends on how the plugin got here. In an engine build it is the
/// application directory; installed from a package it is <store>/<id>/<version>/Bin/<config>.
/// Asking the module where it lives covers both without either layout having to know about the
/// other.
struct PL_EDITORPLUGINCSHARP_DLL plCSharpEditorPaths
{
  /// \brief The directory holding the "CSharp" payload folder.
  ///
  /// Beside this plugin first, then the application directory. The fallback is not merely
  /// defensive: a bundle with %LoadCopy{true} is loaded from a copy elsewhere, and that copy has
  /// no payload next to it.
  static plStringBuilder FindPayloadRoot();
};
