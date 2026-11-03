#include "SuperEmbedderTypes.h"

#include "SuperEmbedderConvert.h"

FString FSuperEmbedderVector::GetFingerprintHex() const
{
	return Fingerprint.Num() > 0 ? SuperEmbedderUeConvert::FromUtf8(SuperEmbedderUe::HexOf(Fingerprint.GetData(), static_cast<size_t>(Fingerprint.Num()))) : FString();
}

FString FSuperEmbedderVector::GetDigestHex() const
{
	const std::vector<float> V(Values.GetData(), Values.GetData() + Values.Num());
	return SuperEmbedderUeConvert::FromUtf8(SuperEmbedderUe::VectorDigestHex(V));
}

FString LexToString(ESuperEmbedderStatus Status)
{
	return SuperEmbedderUeConvert::FromUtf8(SuperEmbedderUe::StatusName(static_cast<SuperEmbedderUe::EStatus>(static_cast<uint8>(Status))));
}
