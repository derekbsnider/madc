// ---- vocabulary: tokenize + classify (the reference's misc.c) ----------
// First two whitespace words; 5-char case-insensitive match (TOKLEN —
// the vocabulary words are stored pre-truncated); lookup order motions
// -> objects -> actions -> zzword -> numeric. Word classes ride CMD as
// small int constants; ids are the data's NAMES.

long T_NONE = 0;
long T_MOTION = 1;
long T_OBJECT = 2;
long T_ACTION = 3;
long T_NUMERIC = 4;
long T_NOTFOUND = 5;

// Whitespace word count of the whole line (the two-word gate).
long word_count(value &line)
{
	long count = 0;
	long len = line.length();
	bool inword = false;
	long i = 0;
	while ( i < len )
	{
		long c = line.at(i);
		if ( c == ' ' || c == '\t' )
			inword = false;
		else if ( !inword )
		{
			inword = true;
			count = count + 1;
		}
		i = i + 1;
	}
	return count;
}

// Extract the first two raw words into CMD["raw0"] / CMD["raw1"].
void split_words(value &line)
{
	CMD["raw0"] = "";
	CMD["raw1"] = "";
	long len = line.length();
	long i = 0;
	long word = 0;
	while ( i < len && word < 2 )
	{
		while ( i < len && (line.at(i) == ' ' || line.at(i) == '\t') )
			i = i + 1;
		long start = i;
		while ( i < len && line.at(i) != ' ' && line.at(i) != '\t' )
			i = i + 1;
		if ( i > start )
		{
			if ( word == 0 )
				CMD["raw0"] = line.substr(start, i - start);
			else
				CMD["raw1"] = line.substr(start, i - start);
			word = word + 1;
		}
	}
}

// Classify one raw word into (type, id) — id is the vocab NAME.
void classify_word(value &raw, const char *typekey, const char *idkey)
{
	if ( raw.length() == 0 )
	{
		CMD[typekey] = T_NONE;
		CMD[idkey] = "";
		return;
	}
	var full = php::strtolower(raw);
	var tok = full;
	if ( full.length() > 5 )
		tok = full.substr(0, 5);
	if ( php::array_key_exists(tok.c_str(), VOC_M) )
	{
		var ids = VOC_M[tok.c_str()];
		CMD[typekey] = T_MOTION;
		CMD[idkey] = ids[0].c_str();
		return;
	}
	if ( php::array_key_exists(tok.c_str(), VOC_O) )
	{
		var ids = VOC_O[tok.c_str()];
		CMD[typekey] = T_OBJECT;
		CMD[idkey] = ids[0].c_str();
		return;
	}
	if ( php::array_key_exists(tok.c_str(), VOC_A) )
	{
		var ids = VOC_A[tok.c_str()];
		var first = ids[0].c_str();
		// The magic-word placeholder action is special-cased below.
		if ( first != "PART" )
		{
			CMD[typekey] = T_ACTION;
			CMD[idkey] = first;
			return;
		}
	}
	// The reservoir magic word: full case-insensitive match.
	if ( ZZWORD.length() > 0 )
	{
		var zlow = php::strtolower(ZZWORD);
		if ( full == zlow )
		{
			CMD[typekey] = T_ACTION;
			CMD[idkey] = "PART";
			return;
		}
	}
	// Numbers (optional leading minus).
	bool digits = false;
	var h = full.substr(0, 1);
	if ( h == "-" && full.length() > 1 )
		digits = php::ctype_digit(full.substr(1, full.length() - 1));
	else
		digits = php::ctype_digit(full);
	if ( digits )
	{
		CMD[typekey] = T_NUMERIC;
		CMD[idkey] = "";
		return;
	}
	CMD[typekey] = T_NOTFOUND;
	CMD[idkey] = "";
}

void tokenize_command(value &line)
{
	split_words(line);
	var r0 = CMD["raw0"];
	var r1 = CMD["raw1"];
	classify_word(r0, "type0", "id0");
	classify_word(r1, "type1", "id1");
}

void shift_words()
{
	CMD["raw0"] = CMD["raw1"];
	CMD["type0"] = CMD["type1"];
	CMD["id0"] = CMD["id1"];
	CMD["raw1"] = "";
	CMD["type1"] = T_NONE;
	CMD["id1"] = "";
}

void clear_command()
{
	OLDOBJ = CMD["obj"];
	CMD["raw0"] = "";
	CMD["raw1"] = "";
	CMD["type0"] = T_NONE;
	CMD["type1"] = T_NONE;
	CMD["id0"] = "";
	CMD["id1"] = "";
	CMD["verb"] = "";
	CMD["obj"] = "";
}
