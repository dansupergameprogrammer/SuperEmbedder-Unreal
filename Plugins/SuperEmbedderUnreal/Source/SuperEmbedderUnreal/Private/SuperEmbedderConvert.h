#pragma once

// The seam between Unreal types and the engine-free cores (Private/Core/). Every FString, TArray and status
// crosses here, so the cores never see an Unreal header and the UE files never name a core type by accident.

#include "CoreMinimal.h"
#include "SuperEmbedderTypes.h"

#include "SuperEmbedderCoreTypes.h"
#include "SuperEmbedderEncodeService.h"
#include "SuperEmbedderEncoder.h"

#include <memory>
#include <string>
#include <vector>

DECLARE_LOG_CATEGORY_EXTERN(LogSuperEmbedder, Log, All);

namespace SuperEmbedderUeConvert
{
	ESuperEmbedderStatus ToUe(SuperEmbedderUe::EStatus Status);
	ESuperEmbedderEncoderState ToUe(SuperEmbedderUe::EEncoderState State);
	ESuperEmbedderOutcome ToUe(SuperEmbedderUe::EOutcome Outcome);
	ESuperEmbedderVectorKind ToUe(SuperEmbedderUe::EKind Kind);

	// InvalidText (an unpaired surrogate) or EmptyText, else Ok with the UTF-8 bytes.
	SuperEmbedderUe::EStatus ToUtf8(const FString& Text, std::string& Out);
	// For paths and ids, where a conversion failure is refused later by name: never fails, replaces nothing.
	std::string ToUtf8Lossy(const FString& Text);
	FString FromUtf8(const std::string& Utf8);

	FSuperEmbedderVector ToUe(const SuperEmbedderUe::FEmbedding& Vector);
	SuperEmbedderUe::FEmbedding ToCore(const FSuperEmbedderVector& Vector);
	FString FingerprintHex(const SuperEmbedderUe::FFingerprint& Fp);

	// IFileReader over IFileManager, so wide (non-ASCII) paths open on Windows.
	class FUeFileReader final : public SuperEmbedderUe::IFileReader
	{
	public:
		bool Exists(const std::string& Path) const override;
		bool Read(const std::string& Path, uint64_t MaxBytes, std::vector<uint8_t>& Out) const override;
	};

	// IWorkerThreads over FRunnableThread.
	class FUeWorkerThreads final : public SuperEmbedderUe::IWorkerThreads
	{
	public:
		FUeWorkerThreads();
		~FUeWorkerThreads() override;
		void Launch(const char* Name, std::function<void()> Body) override;
		void JoinAll() override;

	private:
		// The worker list lives in the .cpp, where FWorker is complete: a TArray<TUniquePtr<FWorker>> member here
		// instantiates the deleter against the incomplete type in every including TU (MSVC C4150).
		struct FWorker;
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};
}
