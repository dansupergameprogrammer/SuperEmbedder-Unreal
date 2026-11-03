#pragma once

// Text conversion and validation for the encode surface (proposal rev 9, section 2.2; the draft's section 5.4
// "Text conversion"). An FString is UTF-16; an unpaired surrogate refuses InvalidText before any conversion,
// so no converter's replacement rule enters a vector. The rest converts to UTF-8 exactly.

#include "SuperEmbedderCoreTypes.h"

#include <string>

namespace SuperEmbedderUe
{
	// UTF-16 to UTF-8. InvalidText on an unpaired surrogate, EmptyText when the result is empty.
	EStatus Utf16ToUtf8(const char16_t* Text, size_t Length, std::string& Out);

	// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
	bool IsValidUtf8(const char* Data, size_t Size);

	// The id rule for rows and set entries: 1 to kMaxIdBytes bytes of valid UTF-8.
	EStatus CheckId(const std::string& Id);
}
