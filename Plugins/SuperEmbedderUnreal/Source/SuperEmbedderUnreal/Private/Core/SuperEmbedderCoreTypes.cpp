#include "SuperEmbedderCoreTypes.h"

#include "SuperEmbedderSha256.h"

#include <cstring>

namespace SuperEmbedderUe
{
	const char* StatusName(EStatus Status)
	{
		switch (Status)
		{
		case EStatus::Ok: return "Ok";
		case EStatus::EncoderNotReady: return "EncoderNotReady";
		case EStatus::QueueFull: return "QueueFull";
		case EStatus::QueryTooLong: return "QueryTooLong";
		case EStatus::DocumentTooLong: return "DocumentTooLong";
		case EStatus::InvalidText: return "InvalidText";
		case EStatus::InvalidUtf8: return "InvalidUtf8";
		case EStatus::EmptyText: return "EmptyText";
		case EStatus::DuplicateId: return "DuplicateId";
		case EStatus::EmptyTicket: return "EmptyTicket";
		case EStatus::InvalidId: return "InvalidId";
		case EStatus::UnknownTicket: return "UnknownTicket";
		case EStatus::GameThreadBlockingRefused: return "GameThreadBlockingRefused";
		case EStatus::WorkerThreadBlockingRefused: return "WorkerThreadBlockingRefused";
		case EStatus::DifferentModelOpen: return "DifferentModelOpen";
		case EStatus::ModelInUse: return "ModelInUse";
		case EStatus::ModelFileMissing: return "ModelFileMissing";
		case EStatus::TokenizerFileMissing: return "TokenizerFileMissing";
		case EStatus::NotAnSslmArtifact: return "NotAnSslmArtifact";
		case EStatus::ArtifactShaMismatch: return "ArtifactShaMismatch";
		case EStatus::OpenFailed: return "OpenFailed";
		case EStatus::EncodeFailed: return "EncodeFailed";
		case EStatus::FingerprintMismatch: return "FingerprintMismatch";
		case EStatus::DimsMismatch: return "DimsMismatch";
		case EStatus::UncheckedVector: return "UncheckedVector";
		case EStatus::ZeroVector: return "ZeroVector";
		case EStatus::NonFiniteVector: return "NonFiniteVector";
		case EStatus::InvalidK: return "InvalidK";
		case EStatus::UnknownId: return "UnknownId";
		case EStatus::WrongThread: return "WrongThread";
		case EStatus::SetCorrupt: return "SetCorrupt";
		case EStatus::BadMagic: return "BadMagic";
		case EStatus::UnsupportedVersion: return "UnsupportedVersion";
		case EStatus::Truncated: return "Truncated";
		case EStatus::Malformed: return "Malformed";
		case EStatus::EntryDigestMismatch: return "EntryDigestMismatch";
		case EStatus::ShuttingDown: return "ShuttingDown";
		case EStatus::InvalidSetting: return "InvalidSetting";
		case EStatus::EncoderClosing: return "EncoderClosing";
		case EStatus::Cancelled: return "Cancelled";
		case EStatus::MissingField: return "MissingField";
		case EStatus::NonStringField: return "NonStringField";
		case EStatus::EmptyInput: return "EmptyInput";
		case EStatus::UnknownHandle: return "UnknownHandle";
		case EStatus::NullSet: return "NullSet";
		case EStatus::Count: break;
		}
		return "Unknown";
	}

	const char* KindName(EKind Kind)
	{
		return Kind == EKind::Query ? "query" : "document";
	}

	std::string HexOf(const uint8_t* Data, size_t Size)
	{
		static const char* const kHex = "0123456789abcdef";
		std::string Out;
		Out.reserve(Size * 2);
		for (size_t I = 0; I < Size; ++I)
		{
			Out.push_back(kHex[Data[I] >> 4]);
			Out.push_back(kHex[Data[I] & 15]);
		}
		return Out;
	}

	void PutU32(std::vector<uint8_t>& Out, uint32_t Value)
	{
		for (int I = 0; I < 4; ++I)
		{
			Out.push_back(static_cast<uint8_t>(Value >> (8 * I)));
		}
	}

	void PutBytes(std::vector<uint8_t>& Out, const void* Data, size_t Size)
	{
		const uint8_t* P = static_cast<const uint8_t*>(Data);
		Out.insert(Out.end(), P, P + Size);
	}

	void PutFloatsLe(std::vector<uint8_t>& Out, const float* Values, size_t Count)
	{
		for (size_t I = 0; I < Count; ++I)
		{
			uint32_t Bits = 0;
			std::memcpy(&Bits, &Values[I], 4);
			PutU32(Out, Bits);
		}
	}

	std::string VectorDigestHex(const std::vector<float>& Values)
	{
		std::vector<uint8_t> Le;
		Le.reserve(Values.size() * 4);
		PutFloatsLe(Le, Values.data(), Values.size());
		const FFingerprint D = Sha256Of(Le.data(), Le.size());
		return HexOf(D);
	}

	uint32_t FByteReader::U32()
	{
		if (!Has(4))
		{
			bTruncated = true;
			return 0;
		}
		uint32_t V = 0;
		for (int I = 0; I < 4; ++I)
		{
			V |= static_cast<uint32_t>(Data[Pos + static_cast<size_t>(I)]) << (8 * I);
		}
		Pos += 4;
		return V;
	}

	uint8_t FByteReader::U8()
	{
		if (!Has(1))
		{
			bTruncated = true;
			return 0;
		}
		return Data[Pos++];
	}

	bool FByteReader::Bytes(void* Out, size_t N)
	{
		if (!Has(N))
		{
			bTruncated = true;
			return false;
		}
		if (N > 0)
		{
			std::memcpy(Out, Data + Pos, N);
		}
		Pos += N;
		return true;
	}

	bool FByteReader::FloatsLe(float* Out, size_t Count)
	{
		if (Count > Remaining() / 4)
		{
			bTruncated = true;
			return false;
		}
		for (size_t I = 0; I < Count; ++I)
		{
			const uint32_t Bits = U32();
			std::memcpy(&Out[I], &Bits, 4);
		}
		return true;
	}
}
