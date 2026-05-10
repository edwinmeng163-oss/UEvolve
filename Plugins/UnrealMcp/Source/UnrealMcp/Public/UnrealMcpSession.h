#pragma once

#include "CoreMinimal.h"

namespace UnrealMcp
{
	void InitializeLaunchSession();
	const FString& GetLaunchSessionId();
	void ShutdownLaunchSession();
}
