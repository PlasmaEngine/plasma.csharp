#include <CSharpPlugin/CSharpPluginPCH.h>
#include <CSharpPlugin/Runtime/CSharpObjectRegistry.h>
#include <Core/GameApplication/GameApplicationBase.h>
#include <Core/Scripting/ScriptComponent.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/JSONWriter.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GameEngine/GameState/GameState.h>
#include <GameEngine/SaveGame/Implementation/SaveGameWire.h>
#include <GameEngine/SaveGame/SaveGame.h>

namespace
{
  class ScriptSaveAdapter final : public plPersistenceAdapter
  {
  public:
    plString schema;
    plUInt32 version = 1;
    plDynamicArray<plString> fields;
    plStringView GetSchema() const override { return schema; }
    plUInt32 GetVersion() const override { return version; }
    plResult Capture(const plGameObject& object,
      plAbstractObjectNode& node) const override
    {
      plHybridArray<const plScriptComponent*, 2> scripts;
      object.TryGetComponentsOfBaseType(scripts);
      if (scripts.GetCount() != 1)
        return PL_FAILURE;
      for (const auto& field : fields)
      {
        plHashedString key;
        key.Assign(field);
        auto value = scripts[0]->GetScriptVariable(key);
        if (!value.IsValid())
          return PL_FAILURE;
        node.AddProperty(field, value);
      }
      return PL_SUCCESS;
    }
    plResult Validate(const plAbstractObjectNode& node) const override
    {
      for (const auto& field : fields)
        if (!node.FindProperty(field))
          return PL_FAILURE;
      return PL_SUCCESS;
    }
    plResult Restore(plGameObject& object,
      const plAbstractObjectNode& node) const override
    {
      plHybridArray<plScriptComponent*, 2> scripts;
      object.TryGetComponentsOfBaseType(scripts);
      if (scripts.GetCount() != 1 || !scripts[0]->GetScriptInstance())
        return PL_FAILURE;
      for (const auto& field : fields)
      {
        plHashedString key;
        key.Assign(field);
        const auto& value = node.FindProperty(field)->m_Value;
        scripts[0]->SetScriptVariable(key, value);
        if (scripts[0]->GetScriptVariable(key) != value)
          return PL_FAILURE;
      }
      return PL_SUCCESS;
    }
  };
  struct ManagedSaveJob
  {
    plSaveGame::Operation operation;
    plUniquePtr<plSaveGameRestore> restore;
    plString output;
    plWorld* queuedWorld = nullptr;
    plWorld* sourceWorld = nullptr;
    bool loadWorld = false;
    plString contentVersion;
  };
  struct ManagedSaveSession
  {
    plSharedPtr<plSaveGame> service;
    plGameState* owner = nullptr;
    plMap<plUInt64, plUniquePtr<ManagedSaveJob>> jobs;
    plUInt64 nextJob = 1;
  };
  plMap<plUInt64, plUniquePtr<ManagedSaveSession>> s_SaveSessions;
  plUInt64 s_NextSaveSession = 1;
  plGameState* ActiveGameState()
  {
    auto* app = plGameApplicationBase::GetGameApplicationBaseInstance();
    return app ? plDynamicCast<plGameState*>(app->GetActiveGameState())
               : nullptr;
  }
  ManagedSaveSession* Session(plUInt64 id)
  {
    if (!plThreadUtils::IsMainThread())
      return nullptr;
    auto it = s_SaveSessions.Find(id);
    if (!it.IsValid())
      return nullptr;
    auto* session = it.Value().Borrow();
    if (session->owner)
    {
      auto* active = ActiveGameState();
      if (active != session->owner ||
          &active->GetSaveGame() != session->service.Borrow())
        return nullptr;
    }
    return session;
  }
  plString Text(const plVariantDictionary& d, plStringView key)
  {
    const auto* p = d.GetValue(key);
    return p && p->IsA<plString>() ? p->Get<plString>() : plString();
  }
  bool Id(const plVariantDictionary& d, plStringView key, plUuid& id,
    bool optional = false)
  {
    const auto text = Text(d, key);
    if (optional && text.IsEmpty())
      return true;
    if (text.GetElementCount() != 36)
      return false;
    const char* chars = text.GetData();
    for (plUInt32 i = 0; i < 36; ++i)
    {
      if (i == 8 || i == 13 || i == 18 || i == 23)
      {
        if (chars[i] != '-')
          return false;
      }
      else if (!((chars[i] >= '0' && chars[i] <= '9') ||
                 (chars[i] >= 'a' && chars[i] <= 'f') ||
                 (chars[i] >= 'A' && chars[i] <= 'F')))
        return false;
    }
    plStringBuilder engineFormat("{ ", text, " }");
    return plConversionUtils::TryConvertStringToUuid(engineFormat, id);
  }
  void IdJson(plStandardJSONWriter& writer, plStringView name, const plUuid& id)
  {
    plStringBuilder s;
    plConversionUtils::ToString(id, s);
    writer.AddVariableString(name, s.GetView().GetSubString(2, 36));
  }
  void MetadataJson(plStandardJSONWriter& w, const plSaveGameMetadata& m)
  {
    w.BeginObject();
    IdJson(w, "profile", m.m_Profile);
    IdJson(w, "slot", m.m_Slot);
    IdJson(w, "revision", m.m_Revision);
    w.AddVariableString("name", m.m_sDisplayName);
    w.AddVariableString("scene", m.m_sScene);
    w.AddVariableString("project", m.m_sProject);
    w.AddVariableString("contentVersion", m.m_sContentVersion);
    w.AddVariableUInt32("kind", static_cast<plUInt32>(m.m_Kind));
    w.AddVariableInt64("utcMicroseconds", m.m_iUtcMicroseconds);
    w.AddVariableDouble("playTimeSeconds", m.m_fPlayTimeSeconds);
    w.EndObject();
  }
} // namespace
extern "C" PL_CSHARPPLUGIN_DLL plUInt32
plSaveGameManagedOpen(const char* project, plUInt64* output)
{
  if (!plThreadUtils::IsMainThread() || !project || !output)
    return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
  plUniquePtr<ManagedSaveSession> session = PL_DEFAULT_NEW(ManagedSaveSession);
  session->owner = ActiveGameState();
  if (session->owner)
    session->service = plSharedPtr<plSaveGame>(
      &session->owner->GetSaveGame(), plFoundation::GetDefaultAllocator());
  else
    session->service = PL_DEFAULT_NEW(plSaveGame);
  auto result = plSaveGameError::Success;
  if (session->service->IsInitialized())
  {
    if (project[0] != '\0' &&
        !session->service->GetProjectName().IsEqual_NoCase(project))
      result = plSaveGameError::Conflict;
  }
  else
    result = session->service->Initialize(project);
  if (result == plSaveGameError::Success)
  {
    *output = s_NextSaveSession++;
    s_SaveSessions.Insert(*output, std::move(session));
  }
  return static_cast<plUInt32>(result);
}
extern "C" PL_CSHARPPLUGIN_DLL void plSaveGameManagedClose(plUInt64 id)
{
  if (!plThreadUtils::IsMainThread())
    return;
  auto it = s_SaveSessions.Find(id);
  if (!it.IsValid())
    return;
  for (auto job = it.Value()->jobs.GetIterator(); job.IsValid(); ++job)
  {
    job.Value()->operation->Cancel();
    if (job.Value()->restore)
      job.Value()->restore->Cancel();
  }
  s_SaveSessions.Remove(id);
}
extern "C" PL_CSHARPPLUGIN_DLL plUInt32 plSaveGameManagedRequest(
  plUInt64 id, const plUInt8* json, plUInt32 length, plUInt64* output)
{
  auto* session = Session(id);
  if (!session || !json || length > 3 * 1024 * 1024 || !output)
    return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
  plRawMemoryStreamReader stream(json, length);
  plJSONReader reader;
  if (reader.Parse(stream).Failed() ||
      reader.GetTopLevelElementType() != plJSONReader::ElementType::Dictionary)
    return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
  const auto& d = reader.GetTopLevelObject();
  const auto command = Text(d, "command");
  if (command == "registerScript")
  {
    if (!session->jobs.IsEmpty())
      return static_cast<plUInt32>(plSaveGameError::Busy);
    plUniquePtr<ScriptSaveAdapter> adapter = PL_DEFAULT_NEW(ScriptSaveAdapter);
    adapter->schema = Text(d, "schema");
    const auto* version = d.GetValue("version");
    const auto* fields = d.GetValue("fields");
    if (!version || !version->CanConvertTo<plUInt32>() || !fields ||
        !fields->IsA<plVariantArray>())
      return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
    adapter->version = version->ConvertTo<plUInt32>();
    for (const auto& field : fields->Get<plVariantArray>())
    {
      if (!field.IsA<plString>() || field.Get<plString>().IsEmpty() ||
          field.Get<plString>() == "Object")
        return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
      adapter->fields.PushBack(field.Get<plString>());
    }
    if (session->service->RegisterOwnedAdapter(std::move(adapter)).Failed())
      return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
    *output = 0;
    return 0;
  }
  if (session->jobs.GetCount() >= 32)
    return static_cast<plUInt32>(plSaveGameError::Busy);
  plUuid profile, slot, expected;
  if (!Id(d, "profile", profile, command == "profiles") ||
      !Id(d, "slot", slot, true) || !Id(d, "expectedRevision", expected, true))
    return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
  plUniquePtr<ManagedSaveJob> job = PL_DEFAULT_NEW(ManagedSaveJob);
  if (command == "save" || command == "checkpoint")
  {
    auto* world = plCSharpExecutionScope::GetCurrentWorld();
    if (!world)
      return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
    plSaveGameMetadata m;
    m.m_Profile = profile;
    m.m_Slot = slot;
    m.m_sDisplayName = Text(d, "name");
    m.m_sScene = Text(d, "scene");
    m.m_sContentVersion = Text(d, "contentVersion");
    m.m_sCheckpoint = Text(d, "checkpoint");
    if (m.m_sScene.IsEmpty())
      return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
    if (const auto* time = d.GetValue("playTimeSeconds"))
      m.m_fPlayTimeSeconds = time->ConvertTo<double>();
    if (command == "checkpoint")
    {
      const auto* index = d.GetValue("ringIndex");
      if (!index || !index->CanConvertTo<plUInt32>())
        return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
      job->operation = session->service->RequestCheckpoint(
        *world, m, index->ConvertTo<plUInt32>(), expected);
    }
    else
    {
      plDynamicArray<plUInt8> thumbnail;
      const auto hex = Text(d, "thumbnailHex");
      if (hex.GetElementCount() > 2 * plSaveGameStorage::MaxThumbnailBytes ||
          hex.GetElementCount() % 2 != 0)
        return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
      auto digit = [](char c) -> plInt32 {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        return -1;
      };
      for (plUInt32 i = 0; i < hex.GetElementCount(); i += 2)
      {
        const auto a = digit(hex.GetData()[i]), b = digit(hex.GetData()[i + 1]);
        if (a < 0 || b < 0)
          return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
        thumbnail.PushBack(static_cast<plUInt8>(a * 16 + b));
      }
      job->operation = session->service->Save(*world, m, expected, thumbnail);
    }
  }
  else if (command == "load")
  {
    job->operation = session->service->Load(profile, slot);
    job->loadWorld = slot.IsValid();
    job->sourceWorld = plCSharpExecutionScope::GetCurrentWorld();
    job->contentVersion = Text(d, "contentVersion");
  }
  else if (command == "metadata")
    job->operation = session->service->ReadMetadata(profile, slot);
  else if (command == "thumbnail")
    job->operation = session->service->ReadThumbnail(profile, slot);
  else if (command == "list")
    job->operation = session->service->EnumerateSlots(profile);
  else if (command == "profiles")
    job->operation = session->service->EnumerateProfiles();
  else if (command == "delete")
    job->operation = session->service->DeleteSlot(profile, slot, expected);
  else if (command == "saveProfile")
  {
    plAbstractObjectGraph graph;
    auto* node =
      graph.AddNode(plUuid::MakeStableUuidFromString("ManagedProfile"),
        "plSaveManagedProfile", 1, "Profile");
    node->AddProperty("Json", Text(d, "profileJson"));
    job->operation = session->service->SaveProfile(profile, Text(d, "name"),
      graph, expected);
  }
  else
    return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
  *output = session->nextJob++;
  session->jobs.Insert(*output, std::move(job));
  return 0;
}
extern "C" PL_CSHARPPLUGIN_DLL void
plSaveGameManagedCancel(plUInt64 id, plUInt64 operation)
{
  if (auto* session = Session(id))
  {
    auto it = session->jobs.Find(operation);
    if (!it.IsValid())
      return;
    if (it.Value()->queuedWorld)
      return; // Publication has already been scheduled.
    it.Value()->operation->Cancel();
    if (it.Value()->restore)
      it.Value()->restore->Cancel();
  }
}
extern "C" PL_CSHARPPLUGIN_DLL plUInt32
plSaveGameManagedPoll(plUInt64 id, plUInt64 operation, char* output,
  plUInt32 capacity, plUInt32* required)
{
  auto* session = Session(id);
  if (!session || !required)
    return static_cast<plUInt32>(plSaveGameError::InvalidArgument);
  // Native callbacks may dispose a managed client or replace the game state.
  // Keep the service alive, then reacquire the session before borrowing its jobs.
  auto service = session->service;
  service->Update();
  session = Session(id);
  if (!session)
    return static_cast<plUInt32>(plSaveGameError::Cancelled);
  auto it = session->jobs.Find(operation);
  if (!it.IsValid())
    return static_cast<plUInt32>(plSaveGameError::NotFound);
  auto& job = *it.Value();
  if (!job.operation->IsFinished())
    return static_cast<plUInt32>(plSaveGameError::Busy);
  auto result = job.operation->GetResult();
  if (job.loadWorld && result == plSaveGameError::Success &&
      job.output.IsEmpty())
  {
    if (!job.restore)
      job.restore = PL_DEFAULT_NEW(plSaveGameRestore, *session->service,
        job.operation, job.contentVersion);
    job.restore->Tick();
    if (!job.restore->IsFinished())
      return static_cast<plUInt32>(plSaveGameError::Busy);
    result = job.restore->GetResult();
    if (result == plSaveGameError::Success)
    {
      auto* app = plGameApplicationBase::GetGameApplicationBaseInstance();
      auto* state =
        app ? plDynamicCast<plGameState*>(app->GetActiveGameState())
            : nullptr;
      if (!state)
        result = plSaveGameError::MissingContent;
      else if (!job.queuedWorld && state->GetMainWorld() != job.sourceWorld)
        result = plSaveGameError::Cancelled;
      else if (!job.queuedWorld)
      {
        auto world = job.restore->TakeWorld();
        job.queuedWorld = world.Borrow();
        if (state->QueueSavedWorld(std::move(world)).Succeeded())
          return static_cast<plUInt32>(plSaveGameError::Busy);
        job.queuedWorld = nullptr;
        result = plSaveGameError::Busy;
      }
      else if (state->GetMainWorld() != job.queuedWorld)
        return static_cast<plUInt32>(plSaveGameError::Busy);
    }
  }
  if (job.output.IsEmpty())
  {
    plSaveGameWire::Writer bytes;
    plStandardJSONWriter writer;
    writer.SetOutputStream(&bytes);
    writer.BeginObject();
    writer.AddVariableUInt32("status", static_cast<plUInt32>(result));
    IdJson(writer, "revision", job.operation->GetRevision());
    writer.BeginArray("entries");
    for (const auto& m : job.operation->GetEntries())
      MetadataJson(writer, m);
    writer.EndArray();
    writer.BeginArray("metadata");
    MetadataJson(writer, job.operation->GetDocument().m_Metadata);
    writer.EndArray();
    const auto* node =
      job.operation->GetDocument().m_State.GetNodeByName("Profile");
    if (node && node->FindProperty("Json") &&
        node->FindProperty("Json")->m_Value.IsA<plString>())
      writer.AddVariableString(
        "profileJson", node->FindProperty("Json")->m_Value.Get<plString>());
    writer.BeginArray("thumbnail");
    for (auto byte : job.operation->GetDocument().m_ThumbnailPng)
      writer.WriteUInt32(byte);
    writer.EndArray();
    writer.EndObject();
    job.output =
      plStringView(reinterpret_cast<const char*>(bytes.m_Data.GetData()),
        bytes.m_Data.GetCount());
  }
  *required = job.output.GetElementCount() + 1;
  if (!output || capacity < *required)
    return 0;
  plMemoryUtils::Copy(output, job.output.GetData(), *required);
  session->jobs.Remove(it);
  return 0;
}
