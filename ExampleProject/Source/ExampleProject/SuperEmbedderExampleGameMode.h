#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "SuperEmbedderExampleGameMode.generated.h"

// The project's default game mode (Config/DefaultEngine.ini). It spawns no pawn, and places the example actor
// (SuperEmbedderExample.h) into whichever map is open unless the map already has one, so the example runs on the
// engine's own empty Entry map and the repository carries no map binary.
UCLASS()
class ASuperEmbedderExampleGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ASuperEmbedderExampleGameMode();

	virtual void StartPlay() override;
};

// Draws the example's lines over the viewport. Canvas text, so it shows in every build configuration,
// Shipping included.
UCLASS()
class ASuperEmbedderExampleHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
