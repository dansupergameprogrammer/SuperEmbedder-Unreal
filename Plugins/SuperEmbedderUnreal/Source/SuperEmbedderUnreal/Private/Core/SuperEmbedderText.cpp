#include "SuperEmbedderText.h"

namespace SuperEmbedderUe
{
	EStatus Utf16ToUtf8(const char16_t* Text, size_t Length, std::string& Out)
	{
		Out.clear();
		Out.reserve(Length * 3);
		for (size_t I = 0; I < Length; ++I)
		{
			uint32_t C = static_cast<uint32_t>(Text[I]);
			if (C >= 0xD800u && C <= 0xDBFFu)
			{
				if (I + 1 >= Length)
				{
					return EStatus::InvalidText;
				}
				const uint32_t Low = static_cast<uint32_t>(Text[I + 1]);
				if (Low < 0xDC00u || Low > 0xDFFFu)
				{
					return EStatus::InvalidText;
				}
				C = 0x10000u + ((C - 0xD800u) << 10) + (Low - 0xDC00u);
				++I;
			}
			else if (C >= 0xDC00u && C <= 0xDFFFu)
			{
				return EStatus::InvalidText;
			}
			if (C < 0x80u)
			{
				Out.push_back(static_cast<char>(C));
			}
			else if (C < 0x800u)
			{
				Out.push_back(static_cast<char>(0xC0u | (C >> 6)));
				Out.push_back(static_cast<char>(0x80u | (C & 0x3Fu)));
			}
			else if (C < 0x10000u)
			{
				Out.push_back(static_cast<char>(0xE0u | (C >> 12)));
				Out.push_back(static_cast<char>(0x80u | ((C >> 6) & 0x3Fu)));
				Out.push_back(static_cast<char>(0x80u | (C & 0x3Fu)));
			}
			else
			{
				Out.push_back(static_cast<char>(0xF0u | (C >> 18)));
				Out.push_back(static_cast<char>(0x80u | ((C >> 12) & 0x3Fu)));
				Out.push_back(static_cast<char>(0x80u | ((C >> 6) & 0x3Fu)));
				Out.push_back(static_cast<char>(0x80u | (C & 0x3Fu)));
			}
		}
		return Out.empty() ? EStatus::EmptyText : EStatus::Ok;
	}

	bool IsValidUtf8(const char* Data, size_t Size)
	{
		const unsigned char* P = reinterpret_cast<const unsigned char*>(Data);
		size_t I = 0;
		while (I < Size)
		{
			const unsigned char B = P[I];
			if (B < 0x80)
			{
				++I;
				continue;
			}
			size_t Width = 0;
			uint32_t C = 0;
			uint32_t Min = 0;
			if ((B & 0xE0) == 0xC0)
			{
				Width = 2;
				C = B & 0x1Fu;
				Min = 0x80;
			}
			else if ((B & 0xF0) == 0xE0)
			{
				Width = 3;
				C = B & 0x0Fu;
				Min = 0x800;
			}
			else if ((B & 0xF8) == 0xF0)
			{
				Width = 4;
				C = B & 0x07u;
				Min = 0x10000;
			}
			else
			{
				return false;
			}
			if (Size - I < Width)
			{
				return false;
			}
			for (size_t J = 1; J < Width; ++J)
			{
				const unsigned char Cont = P[I + J];
				if ((Cont & 0xC0) != 0x80)
				{
					return false;
				}
				C = (C << 6) | (Cont & 0x3Fu);
			}
			if (C < Min || C > 0x10FFFFu || (C >= 0xD800u && C <= 0xDFFFu))
			{
				return false;
			}
			I += Width;
		}
		return true;
	}

	EStatus CheckId(const std::string& Id)
	{
		if (Id.empty() || Id.size() > kMaxIdBytes || !IsValidUtf8(Id.data(), Id.size()))
		{
			return EStatus::InvalidId;
		}
		return EStatus::Ok;
	}
}
