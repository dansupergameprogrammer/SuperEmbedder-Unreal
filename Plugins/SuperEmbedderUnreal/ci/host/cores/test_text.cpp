// Text conversion and validation (section 2.2; cell Q-5), and the artifact
// pre-check the open runs off the game thread.

#include "support.h"

#include "SuperEmbedderSha256.h"
#include "SuperEmbedderText.h"

#include <cstdio>
#include <unistd.h>

using namespace SuperEmbedderUe;
using namespace HostSupport;

HOST_TEST(text_utf16_converts_exactly, "Q-5")
{
	const char16_t Ascii[] = u"lantern";
	std::string Out;
	EXPECT_EQ(Utf16ToUtf8(Ascii, 7, Out), EStatus::Ok);
	EXPECT_EQ(Out, std::string("lantern"));
	// U+00E9, U+65E5 and U+1F600 (a surrogate pair): two, three and four UTF-8 bytes.
	const char16_t Mixed[] = {0x00E9, 0x65E5, 0xD83D, 0xDE00};
	EXPECT_EQ(Utf16ToUtf8(Mixed, 4, Out), EStatus::Ok);
	EXPECT_EQ(Out, std::string("\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80"));
}

HOST_TEST(text_unpaired_surrogates_refuse_invalid_text, "Q-5")
{
	std::string Out;
	const char16_t HighAtEnd[] = {u'a', 0xD83D};
	const char16_t LoneLow[] = {u'a', 0xDE00, u'b'};
	const char16_t HighThenLetter[] = {0xD83D, u'b'};
	EXPECT_EQ(Utf16ToUtf8(HighAtEnd, 2, Out), EStatus::InvalidText);
	EXPECT_EQ(Utf16ToUtf8(LoneLow, 3, Out), EStatus::InvalidText);
	EXPECT_EQ(Utf16ToUtf8(HighThenLetter, 2, Out), EStatus::InvalidText);
	EXPECT_EQ(Utf16ToUtf8(u"", 0, Out), EStatus::EmptyText);
}

HOST_TEST(text_utf8_validation_is_strict, "Q-5")
{
	EXPECT_TRUE(IsValidUtf8("caf\xC3\xA9", 5));
	EXPECT_TRUE(IsValidUtf8("\xF4\x8F\xBF\xBF", 4));  // U+10FFFF
	EXPECT_TRUE(!IsValidUtf8("\xC0\xAF", 2));         // overlong '/'
	EXPECT_TRUE(!IsValidUtf8("\xE0\x80\xAF", 3));     // overlong
	EXPECT_TRUE(!IsValidUtf8("\xED\xA0\x80", 3));     // a surrogate
	EXPECT_TRUE(!IsValidUtf8("\xF4\x90\x80\x80", 4)); // above U+10FFFF
	EXPECT_TRUE(!IsValidUtf8("\xC3", 1));             // truncated
	EXPECT_TRUE(!IsValidUtf8("\xFF", 1));
	EXPECT_TRUE(!IsValidUtf8("a\x80", 2));
}

HOST_TEST(text_id_rule, "D-2")
{
	EXPECT_EQ(CheckId("room_01"), EStatus::Ok);
	EXPECT_EQ(CheckId(""), EStatus::InvalidId);
	EXPECT_EQ(CheckId(std::string(kMaxIdBytes, 'x')), EStatus::Ok);
	EXPECT_EQ(CheckId(std::string(kMaxIdBytes + 1, 'x')), EStatus::InvalidId);
	EXPECT_EQ(CheckId("bad\xFF"), EStatus::InvalidId);
}

namespace
{
	std::string WriteTemp(const std::string& Name, const std::string& Bytes)
	{
		const std::string Path = "/tmp/semb_cores_" + std::to_string(getpid()) + "_" + Name;
		std::FILE* F = std::fopen(Path.c_str(), "wb");
		std::fwrite(Bytes.data(), 1, Bytes.size(), F);
		std::fclose(F);
		return Path;
	}
}

HOST_TEST(artifact_precheck_names_each_failure, "L-1 (a model that fails to load names why), MA-1's check half")
{
	FStdFiles Files;
	const std::string Good = WriteTemp("good.sslm", std::string("SSLM\x02rest", 9));
	const std::string Bad = WriteTemp("bad.sslm", "GGUFxxxx");
	FModelSource S;
	std::string Detail;
	S.ModelPath = "/nonexistent/model.sslm";
	S.TokenizerPath = Good;
	EXPECT_EQ(CheckArtifacts(S, Files, Detail), EStatus::ModelFileMissing);
	EXPECT_EQ(Detail, S.ModelPath);
	S.ModelPath = Good;
	S.TokenizerPath = "/nonexistent/tok.sslm";
	EXPECT_EQ(CheckArtifacts(S, Files, Detail), EStatus::TokenizerFileMissing);
	S.TokenizerPath = Bad;
	EXPECT_EQ(CheckArtifacts(S, Files, Detail), EStatus::NotAnSslmArtifact);
	EXPECT_EQ(Detail, Bad);
	S.TokenizerPath = Good;
	EXPECT_EQ(CheckArtifacts(S, Files, Detail), EStatus::Ok);
	S.ExpectedModelSha256 = std::string(64, '0');
	EXPECT_EQ(CheckArtifacts(S, Files, Detail), EStatus::ArtifactShaMismatch);
	S.ExpectedModelSha256 = HexOf(Sha256Of("SSLM\x02rest", 9));
	EXPECT_EQ(CheckArtifacts(S, Files, Detail), EStatus::Ok);
	// Memory sources take the same checks.
	FModelSource M;
	M.ModelBytes = std::make_shared<std::vector<uint8_t>>(std::vector<uint8_t>{'S', 'S', 'L', 'M'});
	M.TokenizerBytes = std::make_shared<std::vector<uint8_t>>(std::vector<uint8_t>{'N', 'O', 'P', 'E'});
	EXPECT_EQ(CheckArtifacts(M, Files, Detail), EStatus::NotAnSslmArtifact);
	std::remove(Good.c_str());
	std::remove(Bad.c_str());
}
