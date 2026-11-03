#include "SuperEmbedderExampleGameMode.h"

#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "SuperEmbedderExample.h"

ASuperEmbedderExampleGameMode::ASuperEmbedderExampleGameMode()
{
	DefaultPawnClass = nullptr;
	HUDClass = ASuperEmbedderExampleHUD::StaticClass();
}

void ASuperEmbedderExampleGameMode::StartPlay()
{
	Super::StartPlay();
	UWorld* World = GetWorld();
	if (World != nullptr && ASuperEmbedderExample::Find(World) == nullptr)
	{
		World->SpawnActor<ASuperEmbedderExample>();
	}
}

void ASuperEmbedderExampleHUD::DrawHUD()
{
	Super::DrawHUD();
	const ASuperEmbedderExample* Example = ASuperEmbedderExample::Find(GetWorld());
	if (Example == nullptr || Canvas == nullptr)
	{
		return;
	}
	float Y = 40.0f;
	DrawText(TEXT("SuperEmbedder-Unreal example: encode text, bake a vector set, top-k"), FLinearColor(0.6f, 0.8f, 1.0f), 40.0f, Y, nullptr, 1.2f);
	Y += 32.0f;
	for (const FString& Line : Example->GetLines())
	{
		DrawText(Line, FLinearColor::White, 40.0f, Y, nullptr, 1.0f);
		Y += 20.0f;
	}
}
