#pragma once

// The bake input (proposal rev 9, section 2.4; re-audit G-31): JSONL, one JSON object per line, each with a
// string "id" and a string "text". Other fields are ignored; blank lines (only spaces, tabs or a CR) are
// skipped. Each refusal names its line, 1-based:
//   Malformed (not one JSON object, or anything after it on the line), MissingField, NonStringField,
//   InvalidUtf8 (the file's bytes, or a \u escape that leaves an unpaired surrogate), InvalidId,
//   DuplicateId, EmptyText, and EmptyInput for a file with no rows.
// The parser is the cores' own, so a DataTable bake and a JSONL bake meet the same row rules (CheckRows).

#include "SuperEmbedderCoreTypes.h"
#include "SuperEmbedderEncodeService.h"

#include <string>
#include <vector>

namespace SuperEmbedderUe
{
	struct FRowsError
	{
		EStatus Status = EStatus::Ok;
		size_t Line = 0; // 1-based; 0 when not about a line
		std::string Detail;

		std::string Describe() const;
	};

	FRowsError ParseJsonlRows(const char* Data, size_t Size, std::vector<FDocumentRow>& OutRows);

	// The rules every bake input meets before an encode: at least one row, valid ids, unique ids, non-empty
	// valid UTF-8 text. Line numbers are row numbers here (1-based).
	FRowsError CheckRows(const std::vector<FDocumentRow>& Rows);
}
