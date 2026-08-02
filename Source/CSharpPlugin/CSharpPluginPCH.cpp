#include <CSharpPlugin/CSharpPluginPCH.h>

#include <CSharpPlugin/CSharpPluginDLL.h>
#include <CSharpPlugin/Hosting/CSharpHost.h>
#include <Foundation/Configuration/Plugin.h>

PL_STATICLINK_LIBRARY(CSharpPlugin)
{
  if (bReturn)
    return;

  PL_STATICLINK_REFERENCE(CSharpPlugin_Hosting_CSharpHost);
  PL_STATICLINK_REFERENCE(CSharpPlugin_Resources_CSharpClassResource);
}
