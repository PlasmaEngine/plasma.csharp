#include <GameEngineTest/GameEngineTestPCH.h>

#if defined(PL_TEST_CSHARP)

#  include <Core/Scripting/Bindings/ScriptBindingManifest.h>
#  include <CSharpPlugin/Hosting/CSharpHost.h>
#  include <GameEngineTest/TestClass/TestClass.h>

class plGameEngineTestCSharp : public plGameEngineTest
{
public:
  virtual const char* GetTestName() const override { return "CSharp"; }

  virtual plGameEngineTestApplication* CreateApplication() override
  {
    return PL_DEFAULT_NEW(plGameEngineTestApplication, "Basics");
  }

protected:
  virtual void SetupSubTests() override
  {
    AddSubTest("ManagedHostM0", 0);
    AddSubTest("PortableBindingSchema", 1);
  }

  virtual plTestAppRun RunSubTest(plInt32 iIdentifier, plUInt32 uiInvocationCount) override
  {
    PL_IGNORE_UNUSED(uiInvocationCount);

    if (iIdentifier == 1)
    {
      plScriptBindingSnapshot runtimeSnapshot;
      PL_TEST_BOOL(plScriptBindingRegistry::CreateSnapshot(runtimeSnapshot).Succeeded());

      plScriptBindingSnapshot editorProxySnapshot = runtimeSnapshot;
      for (plScriptBindingTypeDesc& type : editorProxySnapshot.m_Types)
      {
        if (!type.m_sPluginName.IsEmpty())
        {
          plStringBuilder proxyName("EditorProxy_", type.m_sPluginName);
          type.m_sPluginName = proxyName;
        }
      }

      const plUInt64 uiRuntimeHash =
        plScriptBindingManifest::ComputeSchemaHash(runtimeSnapshot);
      const plUInt64 uiEditorProxyHash =
        plScriptBindingManifest::ComputeSchemaHash(editorProxySnapshot);
      PL_TEST_BOOL(uiRuntimeHash != 0);
      PL_TEST_INT(uiEditorProxyHash, uiRuntimeHash);
      return plTestAppRun::Quit;
    }

    plCSharpHost* pHost = plCSharpHost::GetSingleton();
    PL_TEST_BOOL(pHost != nullptr);
    if (pHost == nullptr)
      return plTestAppRun::Quit;

    // Merely loading CSharpPlugin must not initialize CoreCLR.
    PL_TEST_BOOL(!pHost->IsInitialized());

    // Keep the regular suite quick while still exercising repeated collectible-context teardown.
    // Release validation runs the command-line M0 probe for 1,000 cycles.
    PL_TEST_BOOL(pHost->RunM0StressTest(10).Succeeded());
    PL_TEST_BOOL(pHost->IsInitialized());
    return plTestAppRun::Quit;
  }
};

static plGameEngineTestCSharp s_GameEngineTestCSharp;

#endif
