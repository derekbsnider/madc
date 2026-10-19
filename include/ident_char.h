#ifndef __IDENT_CHAR_H
#define __IDENT_CHAR_H 1

// THE identifier-character test ([lex.name], the basic character set): the
// first character of an identifier, and every following one. It answers what
// `c == '_' || isalpha(c)` / `c == '_' || isalnum(c)` answer in the C locale
// madc runs in, inline — the <cctype> functions are an out-of-line call per
// character, on the lexer's hottest loops — and the same in every locale.

// A letter of the basic character set (`isalpha` in the C locale).
inline bool is_ascii_letter(unsigned char c)
{
	return (unsigned char)((c | 0x20) - 'a') < 26;
}

inline bool is_ident_start(unsigned char c)
{
	return c == '_' || is_ascii_letter(c);
}

inline bool is_ident_char(unsigned char c)
{
	return is_ident_start(c) || (unsigned char)(c - '0') < 10;
}

#endif
