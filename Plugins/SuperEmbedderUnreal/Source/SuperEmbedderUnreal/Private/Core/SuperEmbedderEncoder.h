#pragma once

// The seam between the encode service and the library. IEncoder is one open encoder; IEncoderFactory opens
// one. FSembEncoderFactory is the real one, over semb.h (and SuperEmbedderCore's token-count verb); the host
// suite adds fakes with latches and counters. Every method of IEncoder is called from worker threads, and the
// library's own handle is safe for MaxConcurrency concurrent encodes (semb.h, semb_config.max_concurrency).

#include "SuperEmbedderCoreTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SuperEmbedderUe
{
	// Reads a whole file or its first bytes. The UE layer implements it with IFileManager (wide paths); the host
	// suite with std::FILE. Called on worker threads.
	class IFileReader
	{
	public:
		virtual ~IFileReader() = default;
		virtual bool Exists(const std::string& Path) const = 0;
		// Reads up to MaxBytes (0 = all). False when the file cannot be opened or read.
		virtual bool Read(const std::string& Path, uint64_t MaxBytes, std::vector<uint8_t>& Out) const = 0;
	};

	// Where the model comes from. Paths are UTF-8; the library reads them itself (semb_encoder_open). Bytes go
	// through semb_encoder_open_memory, which copies them before it reads them.
	struct FModelSource
	{
		std::string ModelPath;
		std::string TokenizerPath;
		std::shared_ptr<const std::vector<uint8_t>> ModelBytes;
		std::shared_ptr<const std::vector<uint8_t>> TokenizerBytes;
		// Optional lowercase hex SHA-256s, checked off the game thread before the open (empty = not checked).
		std::string ExpectedModelSha256;
		std::string ExpectedTokenizerSha256;

		bool IsMemory() const { return ModelBytes != nullptr; }
		// The identity DifferentModelOpen compares: the two paths, or the two byte spans' SHA-256s.
		std::string Key() const;
	};

	class IEncoder
	{
	public:
		virtual ~IEncoder() = default; // closes the library handle
		virtual uint32_t Dims() const = 0;
		virtual const FFingerprint& Fingerprint() const = 0;
		// EncodeFailed with LibraryStatus set on a refusal; InvalidUtf8 and EmptyText by name.
		virtual EStatus Encode(EKind Kind, const std::string& Utf8, std::vector<float>& OutValues, std::string& LibraryStatus) = 0;
		// The body-token count (R17's verb). Queries only at SuperEmbedder b52afd0 (SuperEmbedderCoreExtras.h).
		virtual EStatus CountQueryBodyTokens(const std::string& Utf8, int64_t& OutTokens, std::string& LibraryStatus) = 0;
	};

	struct FOpenResult
	{
		EStatus Status = EStatus::OpenFailed;
		std::string Detail; // the library's status name, or the file that failed a check
		std::unique_ptr<IEncoder> Encoder;
	};

	class IEncoderFactory
	{
	public:
		virtual ~IEncoderFactory() = default;
		// Called on a worker thread. Blocks for the whole open (20-38 s on the published model).
		virtual FOpenResult Open(const FModelSource& Source, uint32_t MaxConcurrency) = 0;
	};

	// The off-the-game-thread pre-open check: both files exist, begin with the .sslm magic, and match their
	// expected SHA-256 when one is configured. Memory sources are checked the same way over their bytes.
	EStatus CheckArtifacts(const FModelSource& Source, const IFileReader& Files, std::string& Detail);

	// The real factory, over semb.h. Files serves CheckArtifacts and, when bReadNonAsciiPaths is set, reads a
	// source whose paths are not pure ASCII so the open runs from memory: the library opens paths with a narrow
	// std::ifstream, which MSVC reads in the ANSI code page, not UTF-8. The memory open copies the bytes, so
	// that route peaks at twice the model's size during the open.
	class FSembEncoderFactory final : public IEncoderFactory
	{
	public:
		explicit FSembEncoderFactory(std::shared_ptr<const IFileReader> InFiles, bool bInReadNonAsciiPaths = false)
			: Files(static_cast<std::shared_ptr<const IFileReader>&&>(InFiles)), bReadNonAsciiPaths(bInReadNonAsciiPaths)
		{
		}
		FOpenResult Open(const FModelSource& Source, uint32_t MaxConcurrency) override;

	private:
		std::shared_ptr<const IFileReader> Files;
		bool bReadNonAsciiPaths = false;
	};

	bool IsAscii(const std::string& Text);
}
