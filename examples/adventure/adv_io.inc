// ---- transcript I/O: byte-parity framing per the reference ------------
// Every message prints as \n + text + \n (vspeak's leading blank; null
// or empty messages print NOTHING). Every input line echoes as
// \n + "> line" + \n (the reference echoes under non-tty — the
// transcript IS stdin+stdout). Comment lines (#) are consumed silently.

// Render one message: %d / %s / %S (pluralize by the previous %d) / %V
// substitution plus the floor->ground rewrite when the player is
// outside — the reference's vspeak, verbatim-run batched.
void render_msg(value &out, value &text, long dval, value &sval)
{
	out = "";
	long len = text.length();
	long i = 0;
	long run = 0;
	bool plural = false;
	while ( i < len )
	{
		long c = text.at(i);
		if ( c == '%' && i + 1 < len )
		{
			long f = text.at(i + 1);
			if ( f == 'd' || f == 's' || f == 'S' || f == 'V' )
			{
				if ( i > run )
					out += text.substr(run, i - run);
				if ( f == 'd' )
				{
					out += to_string(dval).c_str();
					plural = dval != 1;
				}
				else if ( f == 's' )
					out += sval;
				else if ( f == 'S' )
				{
					if ( plural )
						out += "s";
				}
				// %V (version banner) is out of fragment
				// scope — pinned or excluded at A10.
				i += 2;
				run = i;
				continue;
			}
		}
		if ( c == 'f' && i + 5 <= len && !inside_loc(LOC) )
		{
			if ( i + 5 == len || text.at(i + 5) == ' '
			     || text.at(i + 5) == '.' )
			{
				var w = text.substr(i, 5);
				if ( w == "floor" )
				{
					if ( i > run )
						out += text.substr(run, i - run);
					out += "ground";
					i += 5;
					run = i;
					continue;
				}
			}
		}
		i += 1;
	}
	if ( i > run )
		out += text.substr(run, i - run);
}

void speak_rendered(value &text)
{
	if ( text.length() == 0 )
		return;
	std::print("\n{}\n", text);
}

void speak_msg(value &text)
{
	var r;
	var none = "";
	render_msg(r, text, 0, none);
	speak_rendered(r);
}

void speak_msg_d(value &text, long d)
{
	var r;
	var none = "";
	render_msg(r, text, d, none);
	speak_rendered(r);
}

void msg_of(value &out, const char *name)
{
	ui::get(out, W, E("adv-messages"), name);
}

void rspeak(const char *name)
{
	var t;
	msg_of(t, name);
	speak_msg(t);
}

void rspeak_d(const char *name, long d)
{
	var t;
	msg_of(t, name);
	speak_msg_d(t, d);
}

// The reference's sspeak lane (DONT_KNOW / DO_WHAT / NO_SEE): plain
// printf %s substitution — NO floor rewrite (the raw word "floor" must
// echo verbatim), NO empty suppression.
void sspeak_s(const char *name, value &s)
{
	var t;
	msg_of(t, name);
	var out;
	out = "";
	long len = t.length();
	long i = 0;
	long run = 0;
	while ( i < len )
	{
		if ( t.at(i) == '%' && i + 1 < len && t.at(i + 1) == 's' )
		{
			if ( i > run )
				out += t.substr(run, i - run);
			out += s;
			i += 2;
			run = i;
			continue;
		}
		i += 1;
	}
	if ( i > run )
		out += t.substr(run, i - run);
	std::print("\n{}\n", out);
}

// One input line: leading blank, comments skipped, EOF -> false, echo.
bool get_input(value &out)
{
	std::print("\n");
	for (;;)
	{
		if ( !cin.good() )
			return false;
		string raw;
		getline(cin, raw);
		if ( !cin.good() && raw.length() == 0 )
			return false;
		var line = raw.c_str();
		if ( line.length() > 0 )
		{
			var h = line.substr(0, 1);
			if ( h == "#" )
				continue;
		}
		std::print("> {}\n", line);
		out = line;
		return true;
	}
}

// First whitespace word of a line (the yes/no reader's sscanf %s).
void first_word(value &out, value &line)
{
	out = "";
	long len = line.length();
	long i = 0;
	while ( i < len && (line.at(i) == ' ' || line.at(i) == '\t') )
		i = i + 1;
	long start = i;
	while ( i < len && line.at(i) != ' ' && line.at(i) != '\t' )
		i = i + 1;
	if ( i > start )
		out = line.substr(start, i - start);
}

// The reference's yes_or_no: re-speaks the question each round; any
// word starting y/n decides; anything else asks again.
bool ask_yes_no(const char *qname, const char *yname, const char *nname)
{
	var q, y, n;
	msg_of(q, qname);
	msg_of(y, yname);
	msg_of(n, nname);
	for (;;)
	{
		speak_msg(q);
		var reply;
		if ( !get_input(reply) )
			exit(0);
		var w;
		first_word(w, reply);
		var lw = php::strtolower(w);
		if ( lw.length() == 0 )
		{
			rspeak("PLEASE_ANSWER");
			continue;
		}
		var h = lw.substr(0, 1);
		if ( h == "y" )
		{
			speak_msg(y);
			return true;
		}
		if ( h == "n" )
		{
			speak_msg(n);
			return false;
		}
		rspeak("PLEASE_ANSWER");
	}
}
