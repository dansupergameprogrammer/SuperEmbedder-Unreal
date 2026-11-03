#include "SuperEmbedderBlueprintLibrary.h"

#include "Engine/DataTable.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

#include "SuperEmbedderConvert.h"
#include "SuperEmbedderVectorSet.h"

#include "SuperEmbedderRowsInput.h"
#include "SuperEmbedderSimilarity.h"
#include "SuperEmbedderVectorSetCore.h"

#include <unordered_set>

namespace
{
	namespace SE = SuperEmbedderUe;
	using namespace SuperEmbedderUeConvert;

	ESuperEmbedderStatus FromRowsError(const SE::FRowsError& Error, FString& OutMessage)
	{
		OutMessage = Error.Status == SE::EStatus::Ok ? FString() : FromUtf8(Error.Describe());
		return ToUe(Error.Status);
	}
}

ESuperEmbedderStatus USuperEmbedderBlueprintLibrary::Cosine(const FSuperEmbedderVector& A, const FSuperEmbedderVector& B, float& OutScore)
{
	OutScore = 0.0f;
	if (!USuperEmbedderVectorSet::IsOwnerThread())
	{
		return ESuperEmbedderStatus::WrongThread;
	}
	return ToUe(SE::Cosine(ToCore(A), ToCore(B), OutScore));
}

ESuperEmbedderStatus USuperEmbedderBlueprintLibrary::TopK(USuperEmbedderVectorSet* Set, const FSuperEmbedderVector& Query, int32 K, const TArray<FString>& ExcludeIds, TArray<FSuperEmbedderHit>& OutHits)
{
	OutHits.Reset();
	if (!USuperEmbedderVectorSet::IsOwnerThread())
	{
		return ESuperEmbedderStatus::WrongThread;
	}
	if (Set == nullptr)
	{
		return ESuperEmbedderStatus::NullSet;
	}
	std::unordered_set<std::string> Exclude;
	for (const FString& Id : ExcludeIds)
	{
		Exclude.insert(ToUtf8Lossy(Id));
	}
	std::vector<SE::FHit> Hits;
	const SE::EStatus Status = SE::TopK(Set->GetCore().Snapshot(), ToCore(Query), K, Exclude.empty() ? nullptr : &Exclude, Hits);
	if (Status == SE::EStatus::Ok)
	{
		OutHits.Reserve(static_cast<int32>(Hits.size()));
		for (const SE::FHit& Hit : Hits)
		{
			FSuperEmbedderHit H;
			H.Id = FromUtf8(Hit.Id);
			H.Score = Hit.Score;
			H.RowIndex = static_cast<int32>(Hit.Row);
			OutHits.Add(MoveTemp(H));
		}
	}
	return ToUe(Status);
}

USuperEmbedderVectorSet* USuperEmbedderBlueprintLibrary::NewVectorSet(UObject* WorldContextObject)
{
	return NewObject<USuperEmbedderVectorSet>(WorldContextObject != nullptr ? WorldContextObject : GetTransientPackage());
}

FString USuperEmbedderBlueprintLibrary::StatusToString(ESuperEmbedderStatus Status)
{
	return LexToString(Status);
}

ESuperEmbedderStatus USuperEmbedderBlueprintLibrary::ParseJsonlRows(const TArray<uint8>& Bytes, TArray<FSuperEmbedderDocumentRow>& OutRows, FString& OutMessage)
{
	OutRows.Reset();
	std::vector<SE::FDocumentRow> Rows;
	const SE::FRowsError Error = SE::ParseJsonlRows(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()), Rows);
	if (Error.Status != SE::EStatus::Ok)
	{
		return FromRowsError(Error, OutMessage);
	}
	OutRows.Reserve(static_cast<int32>(Rows.size()));
	for (const SE::FDocumentRow& Row : Rows)
	{
		FSuperEmbedderDocumentRow Out;
		Out.Id = FromUtf8(Row.Id);
		Out.Text = FromUtf8(Row.Utf8);
		OutRows.Add(MoveTemp(Out));
	}
	OutMessage.Reset();
	return ESuperEmbedderStatus::Ok;
}

ESuperEmbedderStatus USuperEmbedderBlueprintLibrary::RowsFromDataTable(const UDataTable* Table, TArray<FSuperEmbedderDocumentRow>& OutRows, FString& OutMessage)
{
	OutRows.Reset();
	OutMessage.Reset();
	if (Table == nullptr || Table->GetRowStruct() == nullptr)
	{
		OutMessage = TEXT("no DataTable, or a DataTable without a row struct");
		return ESuperEmbedderStatus::EmptyInput;
	}
	const UScriptStruct* RowStruct = Table->GetRowStruct();
	const FProperty* TextProperty = RowStruct->FindPropertyByName(TEXT("Text"));
	const FStrProperty* AsString = CastField<FStrProperty>(TextProperty);
	const FTextProperty* AsText = CastField<FTextProperty>(TextProperty);
	if (AsString == nullptr && AsText == nullptr)
	{
		OutMessage = FString::Printf(TEXT("row struct %s has no string or text property named Text"), *RowStruct->GetName());
		return TextProperty == nullptr ? ESuperEmbedderStatus::MissingField : ESuperEmbedderStatus::NonStringField;
	}
	// GetRowMap keeps the table's row order.
	for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
	{
		FSuperEmbedderDocumentRow Row;
		Row.Id = Pair.Key.ToString();
		Row.Text = AsString != nullptr ? AsString->GetPropertyValue_InContainer(Pair.Value) : AsText->GetPropertyValue_InContainer(Pair.Value).ToString();
		OutRows.Add(MoveTemp(Row));
	}
	return CheckRows(OutRows, OutMessage);
}

ESuperEmbedderStatus USuperEmbedderBlueprintLibrary::CheckRows(const TArray<FSuperEmbedderDocumentRow>& Rows, FString& OutMessage)
{
	std::vector<SE::FDocumentRow> CoreRows;
	CoreRows.reserve(static_cast<size_t>(Rows.Num()));
	for (int32 I = 0; I < Rows.Num(); ++I)
	{
		SE::FDocumentRow Row;
		if (ToUtf8(Rows[I].Id, Row.Id) == SE::EStatus::InvalidText)
		{
			OutMessage = FString::Printf(TEXT("row %d: an unpaired UTF-16 surrogate in the id"), I + 1);
			return ESuperEmbedderStatus::InvalidId;
		}
		if (ToUtf8(Rows[I].Text, Row.Utf8) == SE::EStatus::InvalidText)
		{
			OutMessage = FString::Printf(TEXT("row %d (%s): an unpaired UTF-16 surrogate"), I + 1, *Rows[I].Id);
			return ESuperEmbedderStatus::InvalidText;
		}
		CoreRows.push_back(MoveTemp(Row));
	}
	return FromRowsError(SE::CheckRows(CoreRows), OutMessage);
}
