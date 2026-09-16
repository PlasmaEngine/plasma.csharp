#include <CSharpPlugin/CSharpPluginPCH.h>

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/JSONWriter.h>
#include <Foundation/IO/MemoryStream.h>
#include <GameEngine/GameApplication/GameApplication.h>

// Two-step response retrieval never re-executes a mutation. Each managed caller retrieves
// its response on the same thread before returning; no native object pointer crosses the ABI.
extern "C" PL_CSHARPPLUGIN_DLL plUInt32 plGameSettingsManagedCommand(
  const char* command, const plUInt8* json, plUInt32 size, char* output, plUInt32 capacity, plUInt32* required)
{
  static thread_local plString response;
  if (!required || size > 1024 * 1024)
    return 1;
  if (command)
  {
    auto* app = plGameApplication::GetGameApplicationInstance();
    if (!app)
      return 2;
    plVariantDictionary args;
    if (size)
    {
      if (!json)
        return 1;
      plRawMemoryStreamReader input(json, size);
      plJSONReader reader;
      if (reader.Parse(input).Failed())
        return 1;
      args = reader.GetTopLevelObject();
    }
    auto result = app->GetGameSettings().Execute(command, args);
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter stream(&storage);
    plStandardJSONWriter writer;
    writer.SetOutputStream(&stream);
    writer.WriteVariant(result);
    plMemoryStreamReader input(&storage);
    response.ReadAll(input);
  }
  *required = response.GetElementCount() + 1;
  if (output && capacity >= *required)
    plMemoryUtils::Copy(output, response.GetData(), *required);
  return 0;
}
