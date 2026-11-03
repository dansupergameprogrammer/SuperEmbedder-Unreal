#include "SuperEmbedderRowsInput.h"

#include "SuperEmbedderText.h"

#include <unordered_set>

namespace SuperEmbedderUe
{
	namespace
	{
		struct FParser
		{
			const char* P;
			const char* End;
			bool bBadUtf8 = false;

			void SkipWs()
			{
				while (P < End && (*P == ' ' || *P == '\t' || *P == '\r' || *P == '\n'))
				{
					++P;
				}
			}

			static int Hex(char C)
			{
				if (C >= '0' && C <= '9') return C - '0';
				if (C >= 'a' && C <= 'f') return C - 'a' + 10;
				if (C >= 'A' && C <= 'F') return C - 'A' + 10;
				return -1;
			}

			bool Hex4(uint32_t& Out)
			{
				if (End - P < 4)
				{
					return false;
				}
				Out = 0;
				for (int I = 0; I < 4; ++I)
				{
					const int H = Hex(P[I]);
					if (H < 0)
					{
						return false;
					}
					Out = (Out << 4) | static_cast<uint32_t>(H);
				}
				P += 4;
				return true;
			}

			static void Append(std::string& Out, uint32_t C)
			{
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

			// P at the opening quote.
			bool String(std::string& Out)
			{
				Out.clear();
				++P;
				while (P < End)
				{
					const char C = *P++;
					if (C == '"')
					{
						return true;
					}
					if (static_cast<unsigned char>(C) < 0x20)
					{
						return false;
					}
					if (C != '\\')
					{
						Out.push_back(C);
						continue;
					}
					if (P >= End)
					{
						return false;
					}
					const char E = *P++;
					switch (E)
					{
					case '"': Out.push_back('"'); break;
					case '\\': Out.push_back('\\'); break;
					case '/': Out.push_back('/'); break;
					case 'b': Out.push_back('\b'); break;
					case 'f': Out.push_back('\f'); break;
					case 'n': Out.push_back('\n'); break;
					case 'r': Out.push_back('\r'); break;
					case 't': Out.push_back('\t'); break;
					case 'u':
					{
						uint32_t U = 0;
						if (!Hex4(U))
						{
							return false;
						}
						if (U >= 0xD800u && U <= 0xDBFFu)
						{
							uint32_t Low = 0;
							if (End - P >= 6 && P[0] == '\\' && P[1] == 'u')
							{
								P += 2;
								if (!Hex4(Low))
								{
									return false;
								}
							}
							if (Low < 0xDC00u || Low > 0xDFFFu)
							{
								bBadUtf8 = true;
								return false;
							}
							U = 0x10000u + ((U - 0xD800u) << 10) + (Low - 0xDC00u);
						}
						else if (U >= 0xDC00u && U <= 0xDFFFu)
						{
							bBadUtf8 = true;
							return false;
						}
						Append(Out, U);
						break;
					}
					default:
						return false;
					}
				}
				return false;
			}

			// Skips any JSON value. Depth-bounded so a hostile line cannot exhaust the stack.
			bool SkipValue(int Depth)
			{
				if (Depth > 64)
				{
					return false;
				}
				SkipWs();
				if (P >= End)
				{
					return false;
				}
				std::string Ignored;
				switch (*P)
				{
				case '"':
					return String(Ignored);
				case '{':
				case '[':
				{
					const char Close = *P == '{' ? '}' : ']';
					const bool bObject = *P == '{';
					++P;
					SkipWs();
					if (P < End && *P == Close)
					{
						++P;
						return true;
					}
					for (;;)
					{
						SkipWs();
						if (bObject)
						{
							if (P >= End || *P != '"' || !String(Ignored))
							{
								return false;
							}
							SkipWs();
							if (P >= End || *P != ':')
							{
								return false;
							}
							++P;
						}
						if (!SkipValue(Depth + 1))
						{
							return false;
						}
						SkipWs();
						if (P < End && *P == ',')
						{
							++P;
							continue;
						}
						if (P < End && *P == Close)
						{
							++P;
							return true;
						}
						return false;
					}
				}
				default:
				{
					const char* Start = P;
					while (P < End && (*P == '-' || *P == '+' || *P == '.' || (*P >= '0' && *P <= '9') ||
						(*P >= 'a' && *P <= 'z') || (*P >= 'A' && *P <= 'Z')))
					{
						++P;
					}
					const std::string Word(Start, static_cast<size_t>(P - Start));
					if (Word == "true" || Word == "false" || Word == "null")
					{
						return true;
					}
					if (Word.empty())
					{
						return false;
					}
					const char F = Word[0];
					return F == '-' || (F >= '0' && F <= '9');
				}
				}
			}
		};

		bool IsBlank(const char* B, const char* E)
		{
			for (const char* P = B; P < E; ++P)
			{
				if (*P != ' ' && *P != '\t' && *P != '\r')
				{
					return false;
				}
			}
			return true;
		}

		FRowsError Err(EStatus S, size_t Line, std::string Detail = std::string())
		{
			FRowsError E;
			E.Status = S;
			E.Line = Line;
			E.Detail = static_cast<std::string&&>(Detail);
			return E;
		}

		// One line: one object with string "id" and "text".
		FRowsError ParseLine(const char* B, const char* E, size_t Line, FDocumentRow& Out)
		{
			FParser Ps{B, E};
			Ps.SkipWs();
			if (Ps.P >= E || *Ps.P != '{')
			{
				return Err(EStatus::Malformed, Line, "not a JSON object");
			}
			++Ps.P;
			bool bHasId = false;
			bool bHasText = false;
			Ps.SkipWs();
			if (Ps.P < E && *Ps.P == '}')
			{
				++Ps.P;
			}
			else
			{
				for (;;)
				{
					Ps.SkipWs();
					std::string Key;
					if (Ps.P >= E || *Ps.P != '"' || !Ps.String(Key))
					{
						return Err(Ps.bBadUtf8 ? EStatus::InvalidUtf8 : EStatus::Malformed, Line, "bad key");
					}
					Ps.SkipWs();
					if (Ps.P >= E || *Ps.P != ':')
					{
						return Err(EStatus::Malformed, Line, "expected ':'");
					}
					++Ps.P;
					Ps.SkipWs();
					if (Key == "id" || Key == "text")
					{
						if (Ps.P >= E || *Ps.P != '"')
						{
							return Err(EStatus::NonStringField, Line, "\"" + Key + "\" is not a string");
						}
						std::string Value;
						if (!Ps.String(Value))
						{
							return Err(Ps.bBadUtf8 ? EStatus::InvalidUtf8 : EStatus::Malformed, Line, "bad \"" + Key + "\" string");
						}
						if (Key == "id")
						{
							Out.Id = static_cast<std::string&&>(Value);
							bHasId = true;
						}
						else
						{
							Out.Utf8 = static_cast<std::string&&>(Value);
							bHasText = true;
						}
					}
					else if (!Ps.SkipValue(0))
					{
						return Err(Ps.bBadUtf8 ? EStatus::InvalidUtf8 : EStatus::Malformed, Line, "bad value of \"" + Key + "\"");
					}
					Ps.SkipWs();
					if (Ps.P < E && *Ps.P == ',')
					{
						++Ps.P;
						continue;
					}
					if (Ps.P < E && *Ps.P == '}')
					{
						++Ps.P;
						break;
					}
					return Err(EStatus::Malformed, Line, "expected ',' or '}'");
				}
			}
			Ps.SkipWs();
			if (Ps.P != E)
			{
				return Err(EStatus::Malformed, Line, "text after the object");
			}
			if (!bHasId)
			{
				return Err(EStatus::MissingField, Line, "no \"id\"");
			}
			if (!bHasText)
			{
				return Err(EStatus::MissingField, Line, "no \"text\"");
			}
			return FRowsError();
		}
	}

	std::string FRowsError::Describe() const
	{
		std::string Out = StatusName(Status);
		if (Line > 0)
		{
			Out += " at line " + std::to_string(Line);
		}
		if (!Detail.empty())
		{
			Out += ": " + Detail;
		}
		return Out;
	}

	FRowsError ParseJsonlRows(const char* Data, size_t Size, std::vector<FDocumentRow>& OutRows)
	{
		OutRows.clear();
		const char* P = Data;
		const char* End = Data + Size;
		// A UTF-8 byte order mark is accepted at the very start.
		if (Size >= 3 && static_cast<unsigned char>(P[0]) == 0xEF && static_cast<unsigned char>(P[1]) == 0xBB &&
			static_cast<unsigned char>(P[2]) == 0xBF)
		{
			P += 3;
		}
		std::unordered_set<std::string> Seen;
		size_t Line = 0;
		while (P < End)
		{
			++Line;
			const char* Eol = P;
			while (Eol < End && *Eol != '\n')
			{
				++Eol;
			}
			if (!IsValidUtf8(P, static_cast<size_t>(Eol - P)))
			{
				return Err(EStatus::InvalidUtf8, Line);
			}
			if (!IsBlank(P, Eol))
			{
				FDocumentRow Row;
				FRowsError E = ParseLine(P, Eol, Line, Row);
				if (E.Status != EStatus::Ok)
				{
					return E;
				}
				if (CheckId(Row.Id) != EStatus::Ok)
				{
					return Err(EStatus::InvalidId, Line);
				}
				if (!Seen.insert(Row.Id).second)
				{
					return Err(EStatus::DuplicateId, Line, "id '" + Row.Id + "'");
				}
				if (Row.Utf8.empty())
				{
					return Err(EStatus::EmptyText, Line, "id '" + Row.Id + "'");
				}
				OutRows.push_back(static_cast<FDocumentRow&&>(Row));
			}
			P = Eol < End ? Eol + 1 : End;
		}
		if (OutRows.empty())
		{
			return Err(EStatus::EmptyInput, 0);
		}
		return FRowsError();
	}

	FRowsError CheckRows(const std::vector<FDocumentRow>& Rows)
	{
		if (Rows.empty())
		{
			return Err(EStatus::EmptyInput, 0);
		}
		std::unordered_set<std::string> Seen;
		for (size_t I = 0; I < Rows.size(); ++I)
		{
			const FDocumentRow& R = Rows[I];
			if (CheckId(R.Id) != EStatus::Ok)
			{
				return Err(EStatus::InvalidId, I + 1);
			}
			if (!Seen.insert(R.Id).second)
			{
				return Err(EStatus::DuplicateId, I + 1, "id '" + R.Id + "'");
			}
			if (!IsValidUtf8(R.Utf8.data(), R.Utf8.size()))
			{
				return Err(EStatus::InvalidUtf8, I + 1, "id '" + R.Id + "'");
			}
			if (R.Utf8.empty())
			{
				return Err(EStatus::EmptyText, I + 1, "id '" + R.Id + "'");
			}
		}
		return FRowsError();
	}
}
