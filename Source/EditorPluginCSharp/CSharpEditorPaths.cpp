#include <EditorPluginCSharp/EditorPluginCSharpPCH.h>

#include <EditorPluginCSharp/CSharpEditorPaths.h>
#include <Foundation/Basics/Platform/Win/IncludeWindows.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Strings/StringConversion.h>

namespace
{
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
} // namespace

plStringBuilder plCSharpEditorPaths::FindPayloadRoot()
{
  // Beside the plugin first, then upwards, then the application directory. Walking up matters
  // because plPlugin runs a DLL from a copy at "<plugin dir>/loaded/<N>/", which leaves the module
  // two directories below the payload that shipped with it.
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
