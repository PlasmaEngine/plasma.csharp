#include <CSharpPlugin/CSharpPluginPCH.h>

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/JSONWriter.h>
#include <Foundation/IO/MemoryStream.h>
#include <GameEngine/PlatformServices/PlatformServices.h>

// Runs one plPlatformServices bridge command and hands the JSON response back through the two-step
// required/capacity protocol. A null command only re-reads the thread's last response; nothing is re-executed.
// Returns 0 on success, 1 for bad input, 2 when no platform hub exists.
extern "C" PL_CSHARPPLUGIN_DLL plUInt32 plPlatformManagedCommand(
  const char* command, const plUInt8* json, plUInt32 size, char* output, plUInt32 capacity, plUInt32* required)
{
  static thread_local plString response;
  if (!required || size > 1024 * 1024)
    return 1;
  if (command)
  {
    plPlatformServices* hub = plPlatformServices::GetInstance();
    if (!hub)
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
    auto result = hub->Execute(command, args);
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
