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
		FString GLaunchSessionTaskLabel;

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

	void SetLaunchSessionTaskLabel(const FString& Label)
	{
		FScopeLock Lock(&GLaunchSessionMutex);
		GLaunchSessionTaskLabel = Label.TrimStartAndEnd().Left(2000);
	}

	FString GetLaunchSessionTaskLabel()
	{
		FScopeLock Lock(&GLaunchSessionMutex);
		return GLaunchSessionTaskLabel;
	}

	void ShutdownLaunchSession()
	{
		FScopeLock Lock(&GLaunchSessionMutex);
		GLaunchSessionTaskLabel.Reset();
	}
}
