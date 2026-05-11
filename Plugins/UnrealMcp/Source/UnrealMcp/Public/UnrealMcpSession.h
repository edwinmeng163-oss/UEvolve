#pragma once

#include "CoreMinimal.h"

namespace UnrealMcp
{
	void InitializeLaunchSession();
	const FString& GetLaunchSessionId();
	void SetLaunchSessionTaskLabel(const FString& Label);
	FString GetLaunchSessionTaskLabel();
	void ShutdownLaunchSession();
}
