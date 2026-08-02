#include <EditorPluginCSharp/EditorPluginCSharpPCH.h>

#include <EditorPluginCSharp/CSharpProjectAsset/CSharpProjectDescriptors.h>
#include <Foundation/Containers/Set.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Utilities/ConversionUtils.h>

namespace
{
  const plVariant* GetRequiredValue(const plVariantDictionary& dictionary, plStringView sName)
  {
    const plVariant* pValue = nullptr;
    return dictionary.TryGetValue(sName, pValue) ? pValue : nullptr;
  }

  plStatus ReadString(const plVariantDictionary& dictionary, plStringView sName, plString& out_sValue)
  {
    const plVariant* pValue = GetRequiredValue(dictionary, sName);
    if (pValue == nullptr || !pValue->CanConvertTo<plString>())
      return plStatus(plFmt("C# descriptor property '{}' must be a string.", sName));

    out_sValue = pValue->ConvertTo<plString>();
    return plStatus(PL_SUCCESS);
  }

  plStatus ReadStringMap(const plVariantDictionary& dictionary, plStringView sName, plMap<plString, plString>& out_values)
  {
    out_values.Clear();

    const plVariant* pValue = GetRequiredValue(dictionary, sName);
    if (pValue == nullptr || !pValue->IsA<plVariantDictionary>())
      return plStatus(plFmt("C# descriptor property '{}' must be an object.", sName));

    for (const auto& entry : pValue->Get<plVariantDictionary>())
    {
      if (!entry.Value().CanConvertTo<plString>())
        return plStatus(plFmt("C# descriptor property '{}.{}' must be a string.", sName, entry.Key()));

      out_values.Insert(entry.Key(), entry.Value().ConvertTo<plString>());
    }

    return plStatus(PL_SUCCESS);
  }

  plStatus ReadDefaultValue(const plVariantDictionary& field, plCSharpFieldDefaultValue& out_value)
  {
    const plVariant* pDefault = GetRequiredValue(field, "defaultValue");
    if (pDefault == nullptr || !pDefault->IsA<plVariantDictionary>())
      return plStatus("C# field descriptor 'defaultValue' must be an object.");

    const plVariantDictionary& defaultValue = pDefault->Get<plVariantDictionary>();
    PL_SUCCEED_OR_RETURN(ReadString(defaultValue, "kind", out_value.m_sKind));
    PL_SUCCEED_OR_RETURN(ReadString(defaultValue, "value", out_value.m_sValue));

    out_value.m_Components.Clear();
    const plVariant* pComponents = GetRequiredValue(defaultValue, "components");
    if (pComponents != nullptr && pComponents->IsA<plVariantDictionary>())
    {
      for (const auto& component : pComponents->Get<plVariantDictionary>())
      {
        if (!component.Value().CanConvertTo<plString>())
          return plStatus(plFmt("C# default component '{}' must be a string.", component.Key()));

        out_value.m_Components.Insert(component.Key(), component.Value().ConvertTo<plString>());
      }
    }
    else if (pComponents != nullptr && pComponents->IsValid())
    {
      return plStatus("C# field descriptor 'defaultValue.components' must be an object or null.");
    }

    return plStatus(PL_SUCCESS);
  }

  bool IsHexId(plStringView sValue)
  {
    if (sValue.GetElementCount() != 16)
      return false;

    for (const char* pCharacter = sValue.GetStartPointer(); pCharacter < sValue.GetEndPointer(); ++pCharacter)
    {
      const bool bDigit = *pCharacter >= '0' && *pCharacter <= '9';
      const bool bUpperHex = *pCharacter >= 'A' && *pCharacter <= 'F';
      if (!bDigit && !bUpperHex)
        return false;
    }

    return true;
  }
} // namespace

plUuid plCSharpProjectDescriptors::ComputeSubAssetGuid(const plUuid& projectGuid, const plUuid& persistentGuid)
{
  plUuid result = persistentGuid;
  result.CombineWithSeed(projectGuid);
  return result;
}

plStatus plCSharpProjectDescriptors::Parse(
  plStringView sJson, const plUuid& projectGuid, plString& out_sEntryAssembly, plDynamicArray<plCSharpClassDescriptor>& out_classes)
{
  out_sEntryAssembly.Clear();
  out_classes.Clear();

  if (sJson.IsEmpty())
    return plStatus("The C# descriptor cache is empty.");

  plRawMemoryStreamReader stream(sJson.GetStartPointer(), sJson.GetElementCount());
  plJSONReader reader;
  reader.SetLogInterface(plLog::GetThreadLocalLogSystem());
  if (reader.Parse(stream).Failed() || reader.GetTopLevelElementType() != plJSONReader::ElementType::Dictionary)
    return plStatus("The C# descriptor cache is not valid JSON.");

  const plVariantDictionary& root = reader.GetTopLevelObject();
  const plVariant* pVersion = GetRequiredValue(root, "inspectorVersion");
  if (pVersion == nullptr || !pVersion->CanConvertTo<plUInt32>() || pVersion->ConvertTo<plUInt32>() != 1)
    return plStatus("The C# descriptor cache has an unsupported inspector version.");

  PL_SUCCEED_OR_RETURN(ReadString(root, "entryAssembly", out_sEntryAssembly));
  if (out_sEntryAssembly.IsEmpty() || plPathUtils::IsAbsolutePath(out_sEntryAssembly))
    return plStatus("The C# descriptor entry assembly must be a relative filename.");

  const plVariant* pClasses = GetRequiredValue(root, "classes");
  if (pClasses == nullptr || !pClasses->IsA<plVariantArray>())
    return plStatus("C# descriptor property 'classes' must be an array.");

  plSet<plUuid> persistentGuids;
  plSet<plUuid> subAssetGuids;
  plSet<plString> typeIds;

  for (const plVariant& classValue : pClasses->Get<plVariantArray>())
  {
    if (!classValue.IsA<plVariantDictionary>())
      return plStatus("Every C# class descriptor must be an object.");

    const plVariantDictionary& classDictionary = classValue.Get<plVariantDictionary>();
    plCSharpClassDescriptor& classDescriptor = out_classes.ExpandAndGetRef();

    plString sPersistentGuid;
    PL_SUCCEED_OR_RETURN(ReadString(classDictionary, "persistentGuid", sPersistentGuid));
    classDescriptor.m_PersistentGuid = plConversionUtils::ConvertStringToUuid(sPersistentGuid);
    if (!classDescriptor.m_PersistentGuid.IsValid())
      return plStatus(plFmt("C# class persistent GUID '{}' is invalid.", sPersistentGuid));

    classDescriptor.m_SubAssetGuid = ComputeSubAssetGuid(projectGuid, classDescriptor.m_PersistentGuid);
    if (persistentGuids.Contains(classDescriptor.m_PersistentGuid) || subAssetGuids.Contains(classDescriptor.m_SubAssetGuid))
      return plStatus(plFmt("C# class persistent GUID '{}' is duplicated.", sPersistentGuid));
    persistentGuids.Insert(classDescriptor.m_PersistentGuid);
    subAssetGuids.Insert(classDescriptor.m_SubAssetGuid);

    PL_SUCCEED_OR_RETURN(ReadString(classDictionary, "typeId", classDescriptor.m_sTypeIdHex));
    if (!IsHexId(classDescriptor.m_sTypeIdHex) || typeIds.Contains(classDescriptor.m_sTypeIdHex))
      return plStatus(plFmt("C# class type ID '{}' is invalid or duplicated.", classDescriptor.m_sTypeIdHex));
    typeIds.Insert(classDescriptor.m_sTypeIdHex);

    PL_SUCCEED_OR_RETURN(ReadString(classDictionary, "managedName", classDescriptor.m_sManagedName));
    if (classDescriptor.m_sManagedName.IsEmpty())
      return plStatus("A C# class descriptor has an empty managed name.");

    if (GetRequiredValue(classDictionary, "sourceFile") != nullptr)
    {
      PL_SUCCEED_OR_RETURN(ReadString(classDictionary, "sourceFile", classDescriptor.m_sSourceFile));
      plStringBuilder cleanSourceFile = classDescriptor.m_sSourceFile;
      cleanSourceFile.MakeCleanPath();
      classDescriptor.m_sSourceFile = cleanSourceFile;
      if (classDescriptor.m_sSourceFile.IsEmpty() || plPathUtils::IsAbsolutePath(classDescriptor.m_sSourceFile))
        return plStatus(plFmt("C# class '{}' has an invalid source file.", classDescriptor.m_sManagedName));
    }

    const plVariant* pFields = GetRequiredValue(classDictionary, "fields");
    if (pFields == nullptr || !pFields->IsA<plVariantArray>())
      return plStatus(plFmt("C# class '{}' has no valid fields array.", classDescriptor.m_sManagedName));

    plSet<plString> fieldIds;
    for (const plVariant& fieldValue : pFields->Get<plVariantArray>())
    {
      if (!fieldValue.IsA<plVariantDictionary>())
        return plStatus(plFmt("C# class '{}' contains an invalid field descriptor.", classDescriptor.m_sManagedName));

      const plVariantDictionary& fieldDictionary = fieldValue.Get<plVariantDictionary>();
      plCSharpFieldDescriptor& fieldDescriptor = classDescriptor.m_Fields.ExpandAndGetRef();
      PL_SUCCEED_OR_RETURN(ReadString(fieldDictionary, "id", fieldDescriptor.m_sIdHex));
      PL_SUCCEED_OR_RETURN(ReadString(fieldDictionary, "name", fieldDescriptor.m_sName));
      PL_SUCCEED_OR_RETURN(ReadString(fieldDictionary, "managedType", fieldDescriptor.m_sManagedType));
      PL_SUCCEED_OR_RETURN(ReadDefaultValue(fieldDictionary, fieldDescriptor.m_DefaultValue));
      PL_SUCCEED_OR_RETURN(ReadStringMap(fieldDictionary, "metadata", fieldDescriptor.m_Metadata));

      if (!IsHexId(fieldDescriptor.m_sIdHex) || fieldIds.Contains(fieldDescriptor.m_sIdHex))
        return plStatus(plFmt("C# field ID '{}' on '{}' is invalid or duplicated.", fieldDescriptor.m_sIdHex, classDescriptor.m_sManagedName));
      fieldIds.Insert(fieldDescriptor.m_sIdHex);
      if (fieldDescriptor.m_sName.IsEmpty() || fieldDescriptor.m_sManagedType.IsEmpty())
        return plStatus(plFmt("C# class '{}' has an unnamed or untyped exposed field.", classDescriptor.m_sManagedName));
    }

    classDescriptor.m_Fields.Sort(
      [](const plCSharpFieldDescriptor& a, const plCSharpFieldDescriptor& b)
      { return a.m_sIdHex < b.m_sIdHex; });
  }

  if (out_classes.IsEmpty())
    return plStatus("The C# project contains no [PlasmaScript] classes.");

  out_classes.Sort([](const plCSharpClassDescriptor& a, const plCSharpClassDescriptor& b)
    { return a.m_SubAssetGuid < b.m_SubAssetGuid; });
  return plStatus(PL_SUCCESS);
}
