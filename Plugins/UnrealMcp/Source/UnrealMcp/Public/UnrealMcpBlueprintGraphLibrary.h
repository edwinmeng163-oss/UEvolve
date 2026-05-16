#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UnrealMcpBlueprintGraphLibrary.generated.h"

class UBlueprint;

UCLASS(meta=(ScriptName="UnrealMcpBlueprintGraphLibrary"))
class UNREALMCP_API UUnrealMcpBlueprintGraphLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category="Unreal MCP|Graph", meta=(ScriptName="AddCallFunctionNode"))
	static bool AddCallFunctionNode(
		UBlueprint* Blueprint,
		FName FunctionName,
		const FString& OptionalContextClassPath,
		FVector2D Location,
		FString& OutNodeGuid,
		TArray<FString>& OutPinNames,
		FString& OutFailureReason);

	UFUNCTION(BlueprintCallable, Category="Unreal MCP|Graph", meta=(ScriptName="AddMovementInputCallNode"))
	static bool AddMovementInputCallNode(
		UBlueprint* Blueprint,
		FVector2D Location,
		FString& OutNodeGuid,
		TArray<FString>& OutPinNames,
		FString& OutFailureReason);

	UFUNCTION(BlueprintCallable, Category="Unreal MCP|Graph", meta=(ScriptName="AddControllerYawInputCallNode"))
	static bool AddControllerYawInputCallNode(
		UBlueprint* Blueprint,
		FVector2D Location,
		FString& OutNodeGuid,
		TArray<FString>& OutPinNames,
		FString& OutFailureReason);

	UFUNCTION(BlueprintCallable, Category="Unreal MCP|Graph", meta=(ScriptName="AddControllerPitchInputCallNode"))
	static bool AddControllerPitchInputCallNode(
		UBlueprint* Blueprint,
		FVector2D Location,
		FString& OutNodeGuid,
		TArray<FString>& OutPinNames,
		FString& OutFailureReason);

	UFUNCTION(BlueprintCallable, Category="Unreal MCP|Graph", meta=(ScriptName="AddInputAxisEventNode"))
	static bool AddInputAxisEventNode(
		UBlueprint* Blueprint,
		FName AxisName,
		FVector2D Location,
		FString& OutNodeGuid,
		TArray<FString>& OutPinNames,
		FString& OutFailureReason);

	UFUNCTION(BlueprintCallable, Category="Unreal MCP|Graph", meta=(ScriptName="AddGetActorForwardVectorNode"))
	static bool AddGetActorForwardVectorNode(
		UBlueprint* Blueprint,
		FVector2D Location,
		FString& OutNodeGuid,
		TArray<FString>& OutPinNames,
		FString& OutFailureReason);

	UFUNCTION(BlueprintCallable, Category="Unreal MCP|Graph", meta=(ScriptName="AddGetActorRightVectorNode"))
	static bool AddGetActorRightVectorNode(
		UBlueprint* Blueprint,
		FVector2D Location,
		FString& OutNodeGuid,
		TArray<FString>& OutPinNames,
		FString& OutFailureReason);

	UFUNCTION(BlueprintCallable, Category="Unreal MCP|Graph", meta=(ScriptName="ConnectPinsByGuid"))
	static bool ConnectPinsByGuid(
		UBlueprint* Blueprint,
		const FString& SourceNodeGuid,
		const FString& SourcePinName,
		const FString& TargetNodeGuid,
		const FString& TargetPinName,
		FString& OutFailureReason);

	UFUNCTION(BlueprintCallable, Category="Unreal MCP|Graph", meta=(ScriptName="CompileBlueprint"))
	static bool CompileBlueprint(
		UBlueprint* Blueprint,
		FString& OutFailureReason);
};
