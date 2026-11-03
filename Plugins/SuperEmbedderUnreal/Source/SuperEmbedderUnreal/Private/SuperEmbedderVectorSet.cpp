#include "SuperEmbedderVectorSet.h"

#include "SuperEmbedderConvert.h"
#include "SuperEmbedderVectorSetCore.h"

#include "Serialization/Archive.h"

namespace
{
	namespace SE = SuperEmbedderUe;
	using namespace SuperEmbedderUeConvert;

	// The asset's own framing around the 'SEVS' bytes, so a later format can be told apart.
	constexpr uint32 kAssetBlobTag = 0x31534553u; // "SES1"
}

USuperEmbedderVectorSet::USuperEmbedderVectorSet()
	: Core(MakeShared<SE::FVectorSetCore>())
{
}

bool USuperEmbedderVectorSet::IsOwnerThread()
{
	return IsInGameThread();
}

void USuperEmbedderVectorSet::RefreshSummary()
{
	RowCount = static_cast<int32>(Core->Num());
	Dims = static_cast<int32>(Core->GetDims());
	Fingerprint = Core->HasFingerprint() ? FingerprintHex(Core->GetFingerprint()) : FString();
}

ESuperEmbedderStatus USuperEmbedderVectorSet::Append(const FString& Id, const FString& Text, const FSuperEmbedderVector& Vector)
{
	if (!IsOwnerThread())
	{
		return ESuperEmbedderStatus::WrongThread;
	}
	std::string IdUtf8;
	if (ToUtf8(Id, IdUtf8) != SE::EStatus::Ok)
	{
		return ESuperEmbedderStatus::InvalidId;
	}
	std::string TextUtf8;
	const SE::EStatus TextStatus = ToUtf8(Text, TextUtf8);
	if (TextStatus == SE::EStatus::InvalidText)
	{
		return ESuperEmbedderStatus::InvalidText;
	}
	// A row's text is informational and may be empty (EmptyText is not a refusal here).
	const SE::EStatus Status = Core->Append(IdUtf8, TextUtf8, ToCore(Vector));
	RefreshSummary();
	return ToUe(Status);
}

ESuperEmbedderStatus USuperEmbedderVectorSet::AppendDocuments(const FSuperEmbedderDocumentsResult& Documents, const TArray<FString>& Texts, FString& OutMessage)
{
	OutMessage.Reset();
	if (!IsOwnerThread())
	{
		return ESuperEmbedderStatus::WrongThread;
	}
	if (Documents.Outcome != ESuperEmbedderOutcome::Completed || Documents.Ids.Num() != Documents.Vectors.Num())
	{
		OutMessage = TEXT("the documents result is not a completed encode");
		return ESuperEmbedderStatus::Malformed;
	}
	if (Texts.Num() != 0 && Texts.Num() != Documents.Ids.Num())
	{
		OutMessage = FString::Printf(TEXT("%d texts for %d rows"), Texts.Num(), Documents.Ids.Num());
		return ESuperEmbedderStatus::Malformed;
	}
	// All or nothing: append to a copy, then swap it in.
	TSharedPtr<SE::FVectorSetCore> Next = MakeShared<SE::FVectorSetCore>(*Core);
	for (int32 I = 0; I < Documents.Ids.Num(); ++I)
	{
		std::string IdUtf8;
		std::string TextUtf8;
		if (ToUtf8(Documents.Ids[I], IdUtf8) != SE::EStatus::Ok)
		{
			OutMessage = FString::Printf(TEXT("row %d"), I + 1);
			return ESuperEmbedderStatus::InvalidId;
		}
		if (Texts.Num() != 0 && ToUtf8(Texts[I], TextUtf8) == SE::EStatus::InvalidText)
		{
			OutMessage = FString::Printf(TEXT("row %d (%s)"), I + 1, *Documents.Ids[I]);
			return ESuperEmbedderStatus::InvalidText;
		}
		const SE::EStatus Status = Next->Append(IdUtf8, TextUtf8, ToCore(Documents.Vectors[I]));
		if (Status != SE::EStatus::Ok)
		{
			OutMessage = FString::Printf(TEXT("row %d (%s)"), I + 1, *Documents.Ids[I]);
			return ToUe(Status);
		}
	}
	Core = Next;
	RefreshSummary();
	return ESuperEmbedderStatus::Ok;
}

ESuperEmbedderStatus USuperEmbedderVectorSet::Remove(const FString& Id)
{
	if (!IsOwnerThread())
	{
		return ESuperEmbedderStatus::WrongThread;
	}
	const SE::EStatus Status = Core->Remove(ToUtf8Lossy(Id));
	RefreshSummary();
	return ToUe(Status);
}

void USuperEmbedderVectorSet::Reset()
{
	if (!IsOwnerThread())
	{
		return;
	}
	Core->Reset();
	RefreshSummary();
}

int32 USuperEmbedderVectorSet::Num() const
{
	return static_cast<int32>(Core->Num());
}

bool USuperEmbedderVectorSet::Contains(const FString& Id) const
{
	return Core->Contains(ToUtf8Lossy(Id));
}

TArray<FString> USuperEmbedderVectorSet::GetIds() const
{
	TArray<FString> Ids;
	Ids.Reserve(static_cast<int32>(Core->Num()));
	for (size_t I = 0; I < Core->Num(); ++I)
	{
		Ids.Add(FromUtf8(Core->Row(I).Id));
	}
	return Ids;
}

ESuperEmbedderStatus USuperEmbedderVectorSet::GetText(const FString& Id, FString& OutText) const
{
	OutText.Reset();
	const int64_t Index = Core->IndexOf(ToUtf8Lossy(Id));
	if (Index < 0)
	{
		return ESuperEmbedderStatus::UnknownId;
	}
	OutText = FromUtf8(Core->Row(static_cast<size_t>(Index)).Text);
	return ESuperEmbedderStatus::Ok;
}

ESuperEmbedderStatus USuperEmbedderVectorSet::GetRowVector(const FString& Id, FSuperEmbedderVector& OutVector) const
{
	OutVector = FSuperEmbedderVector();
	const int64_t Index = Core->IndexOf(ToUtf8Lossy(Id));
	if (Index < 0)
	{
		return ESuperEmbedderStatus::UnknownId;
	}
	OutVector = ToUe(Core->RowVector(static_cast<size_t>(Index)));
	return ESuperEmbedderStatus::Ok;
}

int32 USuperEmbedderVectorSet::GetDims() const
{
	return static_cast<int32>(Core->GetDims());
}

FString USuperEmbedderVectorSet::GetFingerprintHex() const
{
	return Core->HasFingerprint() ? FingerprintHex(Core->GetFingerprint()) : FString();
}

ESuperEmbedderStatus USuperEmbedderVectorSet::SaveToBytes(TArray<uint8>& OutBytes) const
{
	const std::vector<uint8_t> Bytes = Core->Serialize();
	OutBytes.SetNumUninitialized(static_cast<int32>(Bytes.size()));
	FMemory::Memcpy(OutBytes.GetData(), Bytes.data(), Bytes.size());
	return ESuperEmbedderStatus::Ok;
}

ESuperEmbedderStatus USuperEmbedderVectorSet::LoadFromBytes(const TArray<uint8>& Bytes, FString& OutMessage)
{
	OutMessage.Reset();
	if (!IsOwnerThread())
	{
		return ESuperEmbedderStatus::WrongThread;
	}
	TSharedPtr<SE::FVectorSetCore> Next = MakeShared<SE::FVectorSetCore>();
	std::string Detail;
	const SE::EStatus Status = SE::FVectorSetCore::Deserialize(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), *Next, Detail);
	OutMessage = FromUtf8(Detail);
	if (Status == SE::EStatus::Ok)
	{
		Core = Next;
		RefreshSummary();
	}
	return ToUe(Status);
}

void USuperEmbedderVectorSet::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	// Reference collectors and memory counters walk every object; the rows hold no object reference.
	if (Ar.IsObjectReferenceCollector() || Ar.IsCountingMemory())
	{
		return;
	}
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		return;
	}

	uint32 Tag = kAssetBlobTag;
	Ar << Tag;
	if (Ar.IsLoading() && Tag != kAssetBlobTag)
	{
		Ar.SetError();
		LoadStatus = ESuperEmbedderStatus::BadMagic;
		LoadMessage = TEXT("not a SuperEmbedder vector set asset blob");
		return;
	}

	TArray<uint8> Bytes;
	if (Ar.IsSaving())
	{
		const std::vector<uint8_t> Saved = Core->Serialize();
		Bytes.SetNumUninitialized(static_cast<int32>(Saved.size()));
		FMemory::Memcpy(Bytes.GetData(), Saved.data(), Saved.size());
	}
	Ar << Bytes;

	if (Ar.IsLoading())
	{
		// The row digest is recomputed here, on whichever thread the engine loads the asset on (V-8).
		TSharedPtr<SE::FVectorSetCore> Next = MakeShared<SE::FVectorSetCore>();
		std::string Detail;
		const SE::EStatus Status = SE::FVectorSetCore::Deserialize(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), *Next, Detail);
		LoadStatus = ToUe(Status);
		LoadMessage = FromUtf8(Detail);
		Core = Status == SE::EStatus::Ok ? Next : MakeShared<SE::FVectorSetCore>();
		RefreshSummary();
		if (Status != SE::EStatus::Ok)
		{
			UE_LOG(LogSuperEmbedder, Error, TEXT("SuperEmbedder: vector set %s refused on load: %s %s"), *GetPathName(),
				*LexToString(LoadStatus), *LoadMessage);
		}
	}
}
