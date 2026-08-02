#pragma once

#include <Foundation/Basics.h>
#include <Foundation/Configuration/Plugin.h>

#if PL_ENABLED(PL_COMPILE_ENGINE_AS_DLL)
#  ifdef BUILDSYSTEM_BUILDING_EDITORPLUGINCSHARP_LIB
#    define PL_EDITORPLUGINCSHARP_DLL PL_DECL_EXPORT
#  elif defined(BUILDSYSTEM_FOLDING_PLUGIN_IMPORTS)
#    define PL_EDITORPLUGINCSHARP_DLL
#  else
#    define PL_EDITORPLUGINCSHARP_DLL PL_DECL_IMPORT
#  endif
#else
#  define PL_EDITORPLUGINCSHARP_DLL
#endif
