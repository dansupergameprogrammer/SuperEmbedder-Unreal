#include "SuperEmbedderEncoder.h"

#include "SuperEmbedderCoreExtras.h"
#include "SuperEmbedderSha256.h"
#include "SuperEmbedderText.h"

#include <cstring>

namespace SuperEmbedderUe
{
	std::string FModelSource::Key() const
	{
		if (IsMemory())
		{
			const FFingerprint M = Sha256Of(ModelBytes->data(), ModelBytes->size());
			const std::vector<uint8_t> Empty;
			const std::vector<uint8_t>& Tok = TokenizerBytes ? *TokenizerBytes : Empty;
			const FFingerprint T = Sha256Of(Tok.data(), Tok.size());
			return "memory:" + HexOf(M) + ":" + HexOf(T);
		}
		return "path:" + ModelPath + "|" + TokenizerPath;
	}

	namespace
	{
		constexpr uint8_t kSslmMagic[4] = {'S', 'S', 'L', 'M'};

		bool HasMagic(const std::vector<uint8_t>& Bytes)
		{
			return Bytes.size() >= 4 && std::memcmp(Bytes.data(), kSslmMagic, 4) == 0;
		}

		EStatus CheckOne(const std::string& Path, const std::shared_ptr<const std::vector<uint8_t>>& Memory,
			const std::string& ExpectedSha, const IFileReader& Files, EStatus MissingStatus, std::string& Detail)
		{
			std::vector<uint8_t> Read;
			const std::vector<uint8_t>* Bytes = Memory.get();
			if (Bytes == nullptr)
			{
				// Only the head is needed unless a hash is configured.
				if (Path.empty() || !Files.Exists(Path) || !Files.Read(Path, ExpectedSha.empty() ? 4u : 0u, Read))
				{
					Detail = Path;
					return MissingStatus;
				}
				Bytes = &Read;
			}
			if (!HasMagic(*Bytes))
			{
				Detail = Memory ? std::string("(memory)") : Path;
				return EStatus::NotAnSslmArtifact;
			}
			if (!ExpectedSha.empty())
			{
				const std::string Actual = HexOf(Sha256Of(Bytes->data(), Bytes->size()));
				if (Actual != ExpectedSha)
				{
					Detail = (Memory ? std::string("(memory)") : Path) + " sha256 " + Actual + ", expected " + ExpectedSha;
					return EStatus::ArtifactShaMismatch;
				}
			}
			return EStatus::Ok;
		}

		class FSembEncoder final : public IEncoder
		{
		public:
			FSembEncoder(semb_encoder InHandle, uint32_t InDims, const FFingerprint& InFp)
				: Handle(InHandle), DimsValue(InDims), Fp(InFp)
			{
			}
			~FSembEncoder() override
			{
				if (Handle != nullptr)
				{
					semb_encoder_close(Handle);
				}
			}
			uint32_t Dims() const override { return DimsValue; }
			const FFingerprint& Fingerprint() const override { return Fp; }

			EStatus Encode(EKind Kind, const std::string& Utf8, std::vector<float>& OutValues, std::string& LibraryStatus) override
			{
				// semb_encode requires a 16-byte-aligned output; a std::vector<float> is only guaranteed 4.
				std::vector<float> Buffer(static_cast<size_t>(DimsValue) + 4);
				const uintptr_t Raw = reinterpret_cast<uintptr_t>(Buffer.data());
				float* Aligned = reinterpret_cast<float*>((Raw + 15u) & ~static_cast<uintptr_t>(15u));
				size_t Required = 0;
				semb_fingerprint OutFp{};
				OutFp.struct_size = sizeof(OutFp);
				const semb_status S = (Kind == EKind::Query ? semb_encode_query : semb_encode)(Handle, Utf8.data(),
					Utf8.size(), Aligned, static_cast<size_t>(DimsValue) * sizeof(float), &Required, &OutFp);
				if (S != SEMB_OK)
				{
					LibraryStatus = semb_status_name(S);
					if (S == SEMB_INVALID_UTF8)
					{
						return EStatus::InvalidUtf8;
					}
					if (S == SEMB_INPUT_EMPTY)
					{
						return EStatus::EmptyText;
					}
					return EStatus::EncodeFailed;
				}
				if (std::memcmp(OutFp.digest, Fp.data(), 32) != 0)
				{
					// The handle's fingerprint never changes while it is open; a defensive check only.
					LibraryStatus = "fingerprint changed under an open handle";
					return EStatus::FingerprintMismatch;
				}
				OutValues.assign(Aligned, Aligned + DimsValue);
				return EStatus::Ok;
			}

			EStatus CountQueryBodyTokens(const std::string& Utf8, int64_t& OutTokens, std::string& LibraryStatus) override
			{
				const semb_status S = semb_ue_query_body_token_count(Handle, Utf8.data(), Utf8.size(), &OutTokens);
				if (S != SEMB_OK)
				{
					LibraryStatus = semb_status_name(S);
					return EStatus::EncodeFailed;
				}
				return EStatus::Ok;
			}

		private:
			semb_encoder Handle = nullptr;
			uint32_t DimsValue = 0;
			FFingerprint Fp{};
		};
	}

	EStatus CheckArtifacts(const FModelSource& Source, const IFileReader& Files, std::string& Detail)
	{
		const EStatus M = CheckOne(Source.ModelPath, Source.ModelBytes, Source.ExpectedModelSha256, Files,
			EStatus::ModelFileMissing, Detail);
		if (M != EStatus::Ok)
		{
			return M;
		}
		return CheckOne(Source.TokenizerPath, Source.TokenizerBytes, Source.ExpectedTokenizerSha256, Files,
			EStatus::TokenizerFileMissing, Detail);
	}

	bool IsAscii(const std::string& Text)
	{
		for (char C : Text)
		{
			if (static_cast<unsigned char>(C) >= 0x80)
			{
				return false;
			}
		}
		return true;
	}

	FOpenResult FSembEncoderFactory::Open(const FModelSource& InSource, uint32_t MaxConcurrency)
	{
		FOpenResult Result;
		FModelSource Source = InSource;
		if (bReadNonAsciiPaths && !Source.IsMemory() && (!IsAscii(Source.ModelPath) || !IsAscii(Source.TokenizerPath)))
		{
			std::shared_ptr<std::vector<uint8_t>> Model = std::make_shared<std::vector<uint8_t>>();
			std::shared_ptr<std::vector<uint8_t>> Tok = std::make_shared<std::vector<uint8_t>>();
			if (!Files->Read(Source.ModelPath, 0, *Model))
			{
				Result.Status = EStatus::ModelFileMissing;
				Result.Detail = Source.ModelPath;
				return Result;
			}
			if (!Files->Read(Source.TokenizerPath, 0, *Tok))
			{
				Result.Status = EStatus::TokenizerFileMissing;
				Result.Detail = Source.TokenizerPath;
				return Result;
			}
			Source.ModelBytes = Model;
			Source.TokenizerBytes = Tok;
		}
		Result.Status = CheckArtifacts(Source, *Files, Result.Detail);
		if (Result.Status != EStatus::Ok)
		{
			return Result;
		}
		semb_encoder Handle = nullptr;
		semb_status S = SEMB_OK;
		if (Source.IsMemory())
		{
			semb_memory_config Cfg{};
			Cfg.struct_size = sizeof(Cfg);
			Cfg.model_bytes = Source.ModelBytes->data();
			Cfg.model_len = Source.ModelBytes->size();
			Cfg.tokenizer_bytes = Source.TokenizerBytes ? Source.TokenizerBytes->data() : nullptr;
			Cfg.tokenizer_len = Source.TokenizerBytes ? Source.TokenizerBytes->size() : 0;
			Cfg.max_concurrency = MaxConcurrency;
			S = semb_encoder_open_memory(&Cfg, &Handle);
		}
		else
		{
			semb_config Cfg{};
			Cfg.struct_size = sizeof(Cfg);
			Cfg.model_path = Source.ModelPath.data();
			Cfg.model_path_len = Source.ModelPath.size();
			Cfg.tokenizer_path = Source.TokenizerPath.data();
			Cfg.tokenizer_path_len = Source.TokenizerPath.size();
			Cfg.max_concurrency = MaxConcurrency;
			Cfg.device = SEMB_DEVICE_CPU;
			S = semb_encoder_open(&Cfg, &Handle);
		}
		if (S != SEMB_OK)
		{
			Result.Status = EStatus::OpenFailed;
			Result.Detail = semb_status_name(S);
			return Result;
		}
		uint32_t Dims = 0;
		semb_fingerprint Fp{};
		Fp.struct_size = sizeof(Fp);
		S = semb_encoder_dims(Handle, &Dims);
		if (S == SEMB_OK)
		{
			S = semb_fingerprint_of(Handle, &Fp);
		}
		if (S != SEMB_OK || Dims == 0)
		{
			semb_encoder_close(Handle);
			Result.Status = EStatus::OpenFailed;
			Result.Detail = S != SEMB_OK ? semb_status_name(S) : "zero dims";
			return Result;
		}
		FFingerprint F{};
		std::memcpy(F.data(), Fp.digest, 32);
		Result.Status = EStatus::Ok;
		Result.Encoder.reset(new FSembEncoder(Handle, Dims, F));
		return Result;
	}
}
