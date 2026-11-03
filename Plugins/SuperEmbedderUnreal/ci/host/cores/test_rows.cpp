// The bake input (section 2.4; re-audit G-31, cell B-5): every refusal by name, with its line.

#include "support.h"

#include "SuperEmbedderRowsInput.h"

using namespace SuperEmbedderUe;

namespace
{
	FRowsError Parse(const std::string& Text, std::vector<FDocumentRow>& Rows)
	{
		return ParseJsonlRows(Text.data(), Text.size(), Rows);
	}
}

HOST_TEST(rows_jsonl_accepts_rows_in_order, "B-1's JSONL half")
{
	std::vector<FDocumentRow> Rows;
	const std::string Text =
		"\xEF\xBB\xBF{\"id\": \"a\", \"text\": \"the drowned monastery\"}\r\n"
		"\n"
		"   \n"
		"{\"text\": \"caf\\u00e9 \\ud83d\\ude00\", \"extra\": [1, {\"x\": null}, true, -2.5e3], \"id\": \"b\"}\n"
		"{\"id\":\"c\",\"text\":\"tab\\tquote\\\"slash\\/\"}";
	const FRowsError E = Parse(Text, Rows);
	REQUIRE_EQ(E.Status, EStatus::Ok);
	REQUIRE_EQ(Rows.size(), size_t(3));
	EXPECT_EQ(Rows[0].Id, std::string("a"));
	EXPECT_EQ(Rows[0].Utf8, std::string("the drowned monastery"));
	EXPECT_EQ(Rows[1].Id, std::string("b"));
	EXPECT_EQ(Rows[1].Utf8, std::string("caf\xC3\xA9 \xF0\x9F\x98\x80"));
	EXPECT_EQ(Rows[2].Utf8, std::string("tab\tquote\"slash/"));
}

HOST_TEST(rows_jsonl_refusals_name_their_line, "B-5")
{
	struct FCase
	{
		const char* Text;
		EStatus Status;
		size_t Line;
	};
	const FCase Cases[] = {
		{"{\"id\":\"a\",\"text\":\"x\"}\n{\"id\":\"b\",\"text\":\"y\"\n", EStatus::Malformed, 2},
		{"{\"id\":\"a\",\"text\":\"x\"} trailing\n", EStatus::Malformed, 1},
		{"[\"id\",\"a\"]\n", EStatus::Malformed, 1},
		{"{\"id\":\"a\"}\n", EStatus::MissingField, 1},
		{"{\"id\":\"a\",\"text\":\"x\"}\n{\"text\":\"y\"}\n", EStatus::MissingField, 2},
		{"{\"id\":\"a\",\"text\":7}\n", EStatus::NonStringField, 1},
		{"{\"id\":3,\"text\":\"x\"}\n", EStatus::NonStringField, 1},
		{"{\"id\":\"a\",\"text\":\"bad \xFF byte\"}\n", EStatus::InvalidUtf8, 1},
		{"{\"id\":\"a\",\"text\":\"lone \\ud83d here\"}\n", EStatus::InvalidUtf8, 1},
		{"{\"id\":\"a\",\"text\":\"x\"}\n{\"id\":\"a\",\"text\":\"y\"}\n", EStatus::DuplicateId, 2},
		{"{\"id\":\"\",\"text\":\"x\"}\n", EStatus::InvalidId, 1},
		{"{\"id\":\"a\",\"text\":\"\"}\n", EStatus::EmptyText, 1},
		{"", EStatus::EmptyInput, 0},
		{"\n  \n", EStatus::EmptyInput, 0},
	};
	for (const FCase& C : Cases)
	{
		std::vector<FDocumentRow> Rows;
		const FRowsError E = Parse(C.Text, Rows);
		EXPECT_EQ(E.Status, C.Status);
		EXPECT_EQ(E.Line, C.Line);
	}
	std::vector<FDocumentRow> Rows;
	EXPECT_EQ(Parse("{\"id\":\"a\",\"text\":7}\n", Rows).Describe(),
		std::string("NonStringField at line 1: \"text\" is not a string"));
}

HOST_TEST(rows_check_rows_applies_the_same_rules, "B-1 (a DataTable and JSONL meet one rule set)")
{
	std::vector<FDocumentRow> Rows = {{"a", "x"}, {"b", "y"}};
	EXPECT_EQ(CheckRows(Rows).Status, EStatus::Ok);
	Rows.push_back({"a", "z"});
	EXPECT_EQ(CheckRows(Rows).Status, EStatus::DuplicateId);
	EXPECT_EQ(CheckRows(Rows).Line, size_t(3));
	EXPECT_EQ(CheckRows({{"a", ""}}).Status, EStatus::EmptyText);
	EXPECT_EQ(CheckRows({{"", "x"}}).Status, EStatus::InvalidId);
	EXPECT_EQ(CheckRows({{"a", "\xC3"}}).Status, EStatus::InvalidUtf8);
	EXPECT_EQ(CheckRows({}).Status, EStatus::EmptyInput);
}
