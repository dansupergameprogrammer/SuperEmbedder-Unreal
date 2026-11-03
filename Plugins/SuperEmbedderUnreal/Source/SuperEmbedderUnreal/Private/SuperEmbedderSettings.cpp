#include "SuperEmbedderSettings.h"

#include "Misc/Paths.h"

FString USuperEmbedderSettings::ResolvePath(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return Path;
	}
	FString Full = FPaths::IsRelative(Path) ? FPaths::Combine(FPaths::ProjectDir(), Path) : Path;
	return FPaths::ConvertRelativePathToFull(Full);
}
