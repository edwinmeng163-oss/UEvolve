#include "UnrealMcpSession.h"

#include "HAL/CriticalSection.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

namespace UnrealMcp
{
	namespace
	{
		FCriticalSection GLaunchSessionMutex;
		FString GLaunchSessionId;

		FString MakeLaunchSessionId()
		{
			const FString TimePart = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
			return FString::Printf(TEXT("%s-%s"), *TimePart, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
		}
	}

	void InitializeLaunchSession()
	{
		FScopeLock Lock(&GLaunchSessionMutex);
		if (GLaunchSessionId.IsEmpty())
		{
			GLaunchSessionId = MakeLaunchSessionId();
		}
	}

	const FString& GetLaunchSessionId()
	{
		InitializeLaunchSession();
		return GLaunchSessionId;
	}

	void ShutdownLaunchSession()
	{
		FScopeLock Lock(&GLaunchSessionMutex);
	}
}
