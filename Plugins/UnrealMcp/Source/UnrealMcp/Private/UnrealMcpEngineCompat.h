#pragma once

// Central Unreal Engine compatibility shim. This is the only plugin file that
// may contain #if ENGINE_*_VERSION; route version differences here. Let this
// file grow until ~200 lines before splitting it into a subdirectory.
#include "Runtime/Launch/Resources/Version.h"

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
#include "Misc/StringOutputDevice.h"
#else
#include "Containers/UnrealString.h"
#endif
