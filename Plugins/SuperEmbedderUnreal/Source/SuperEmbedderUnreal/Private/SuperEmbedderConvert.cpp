#include "SuperEmbedderConvert.h"

#include "HAL/FileManager.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Serialization/Archive.h"

#include "SuperEmbedderText.h"

DEFINE_LOG_CATEGORY(LogSuperEmbedder);

// The Blueprint enum mirrors the cores' enum value for value. A status added to one and not the other fails here.
static_assert(static_cast<int32>(SuperEmbedderUe::EStatus::Count) == static_cast<int32>(ESuperEmbedderStatus::NullSet) + 1,
	"ESuperEmbedderStatus must mirror SuperEmbedderUe::EStatus value for value");
static_assert(static_cast<int32>(SuperEmbedderUe::EStatus::UnknownHandle) == static_cast<int32>(ESuperEmbedderStatus::UnknownHandle), "");
static_assert(static_cast<int32>(SuperEmbedderUe::EStatus::Cancelled) == static_cast<int32>(ESuperEmbedderStatus::Cancelled), "");
static_assert(static_cast<int32>(SuperEmbedderUe::EStatus::WrongThread) == static_cast<int32>(ESuperEmbedderStatus::WrongThread), "");
static_assert(static_cast<int32>(SuperEmbedderUe::EEncoderState::Closing) == static_cast<int32>(ESuperEmbedderEncoderState::Closing), "");
static_assert(sizeof(TCHAR) == sizeof(char16_t), "SuperEmbedder converts FString as UTF-16 (Win64 only)");

namespace SuperEmbedderUeConvert
{
	namespace SE = SuperEmbedderUe;

	ESuperEmbedderStatus ToUe(SE::EStatus Status) { return static_cast<ESuperEmbedderStatus>(static_cast<uint8>(Status)); }
	ESuperEmbedderEncoderState ToUe(SE::EEncoderState State) { return static_cast<ESuperEmbedderEncoderState>(static_cast<uint8>(State)); }
	ESuperEmbedderOutcome ToUe(SE::EOutcome Outcome) { return static_cast<ESuperEmbedderOutcome>(static_cast<uint8>(Outcome)); }
	ESuperEmbedderVectorKind ToUe(SE::EKind Kind) { return static_cast<ESuperEmbedderVectorKind>(static_cast<uint8>(Kind)); }

	SE::EStatus ToUtf8(const FString& Text, std::string& Out)
	{
		return SE::Utf16ToUtf8(reinterpret_cast<const char16_t*>(*Text), static_cast<size_t>(Text.Len()), Out);
	}

	std::string ToUtf8Lossy(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text, Text.Len());
		return std::string(reinterpret_cast<const char*>(Converted.Get()), static_cast<size_t>(Converted.Length()));
	}

	FString FromUtf8(const std::string& Utf8)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Utf8.data()), static_cast<int32>(Utf8.size()));
	}

	FSuperEmbedderVector ToUe(const SE::FEmbedding& Vector)
	{
		FSuperEmbedderVector Out;
		Out.Values.SetNumUninitialized(static_cast<int32>(Vector.Values.size()));
		if (!Vector.Values.empty())
		{
			FMemory::Memcpy(Out.Values.GetData(), Vector.Values.data(), Vector.Values.size() * sizeof(float));
		}
		Out.Kind = ToUe(Vector.Kind);
		Out.bChecked = Vector.bChecked;
		if (Vector.bChecked)
		{
			Out.Fingerprint.Append(Vector.Fingerprint.data(), static_cast<int32>(Vector.Fingerprint.size()));
		}
		return Out;
	}

	SE::FEmbedding ToCore(const FSuperEmbedderVector& Vector)
	{
		std::vector<float> Values(Vector.Values.GetData(), Vector.Values.GetData() + Vector.Values.Num());
		// A vector is checked only with its whole fingerprint; a struct Blueprint or C++ filled in by hand
		// with bChecked but no fingerprint stays unchecked.
		if (Vector.bChecked && Vector.Fingerprint.Num() == 32)
		{
			SE::FFingerprint Fp{};
			FMemory::Memcpy(Fp.data(), Vector.Fingerprint.GetData(), 32);
			return SE::FEmbedding::Checked(MoveTemp(Values), Fp,
				Vector.Kind == ESuperEmbedderVectorKind::Document ? SE::EKind::Document : SE::EKind::Query);
		}
		return SE::FEmbedding::Unchecked(MoveTemp(Values));
	}

	FString FingerprintHex(const SE::FFingerprint& Fp) { return FromUtf8(SE::HexOf(Fp)); }

	// --- Files ---

	bool FUeFileReader::Exists(const std::string& Path) const
	{
		return IFileManager::Get().FileExists(*FromUtf8(Path));
	}

	bool FUeFileReader::Read(const std::string& Path, uint64_t MaxBytes, std::vector<uint8_t>& Out) const
	{
		Out.clear();
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FromUtf8(Path)));
		if (!Reader)
		{
			return false;
		}
		const int64 Total = Reader->TotalSize();
		if (Total < 0)
		{
			return false;
		}
		const uint64_t Want = (MaxBytes == 0 || static_cast<uint64_t>(Total) < MaxBytes) ? static_cast<uint64_t>(Total) : MaxBytes;
		Out.resize(static_cast<size_t>(Want));
		if (Want > 0)
		{
			Reader->Serialize(Out.data(), static_cast<int64>(Want));
		}
		const bool bOk = !Reader->IsError();
		Reader->Close();
		if (!bOk)
		{
			Out.clear();
		}
		return bOk;
	}

	// --- Threads ---

	struct FUeWorkerThreads::FWorker final : public FRunnable
	{
		std::function<void()> Body;
		FRunnableThread* Thread = nullptr;

		uint32 Run() override
		{
			Body();
			return 0;
		}
	};

	struct FUeWorkerThreads::FImpl
	{
		TArray<TUniquePtr<FWorker>> Workers;
	};

	FUeWorkerThreads::FUeWorkerThreads() : Impl(MakeUnique<FImpl>()) {}

	FUeWorkerThreads::~FUeWorkerThreads()
	{
		JoinAll();
	}

	void FUeWorkerThreads::Launch(const char* Name, std::function<void()> Body)
	{
		TUniquePtr<FWorker> Worker = MakeUnique<FWorker>();
		Worker->Body = MoveTemp(Body);
		Worker->Thread = FRunnableThread::Create(Worker.Get(), *FromUtf8(Name), 1024 * 1024, TPri_BelowNormal);
		if (Worker->Thread == nullptr)
		{
			// No thread: run nothing. The service then never leaves its queue state, which Shutdown() handles;
			// a platform that cannot start a thread cannot run the encoder at all.
			UE_LOG(LogSuperEmbedder, Error, TEXT("SuperEmbedder: could not start worker thread %s"), *FromUtf8(Name));
			return;
		}
		Impl->Workers.Add(MoveTemp(Worker));
	}

	void FUeWorkerThreads::JoinAll()
	{
		for (TUniquePtr<FWorker>& Worker : Impl->Workers)
		{
			if (Worker->Thread != nullptr)
			{
				Worker->Thread->WaitForCompletion();
				delete Worker->Thread;
				Worker->Thread = nullptr;
			}
		}
		Impl->Workers.Reset();
	}
}
