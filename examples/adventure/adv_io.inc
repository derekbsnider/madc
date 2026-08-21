// ---- transcript I/O: byte-parity framing per the reference ------------
// Every message prints as \n + text + \n (vspeak's leading blank; null
// or empty messages print NOTHING). Every input line echoes as
// \n + "> line" + \n (the reference echoes under non-tty — the
// transcript IS stdin+stdout). Comment lines (#) are consumed silently.

// The idx'th integer of a d-args array.
long darg_at(value &a, long idx)
{
	long i = 0;
	for ( value x : a )
	{
		if ( i == idx )
			return x.as_integer();
		i = i + 1;
	}
	return 0;
}

// Render one message: %d (consumes the next d-arg) / %s / %S (pluralize
// by the previous %d) / %V substitution plus the floor->ground rewrite
// when the player is outside — the reference's vspeak, verbatim-run
// batched.
void render_msg(value &out, value &text, value &dargs, value &sval)
{
	out = "";
	long len = text.length();
	long i = 0;
	long run = 0;
	long di = 0;
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
					long dval = darg_at(dargs, di);
					di = di + 1;
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
	var nod;
	render_msg(r, text, nod, none);
	speak_rendered(r);
}

// vspeak with blank=false — inventory listings print consecutively.
void speak_msg_noblank(value &text)
{
	var r;
	var none = "";
	var nod;
	render_msg(r, text, nod, none);
	if ( r.length() == 0 )
		return;
	std::print("{}\n", r);
}

void speak_msg_d(value &text, long d)
{
	var r;
	var none = "";
	var ds;
	php::array_push(ds, d);
	render_msg(r, text, ds, none);
	speak_rendered(r);
}

void speak_msg_d3(value &text, long a, long b, long c)
{
	var r;
	var none = "";
	var ds;
	php::array_push(ds, a);
	php::array_push(ds, b);
	php::array_push(ds, c);
	render_msg(r, text, ds, none);
	speak_rendered(r);
}

void speak_msg_s(value &text, value &s)
{
	var r;
	var nod;
	render_msg(r, text, nod, s);
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

void rspeak_d3(const char *name, long a, long b, long c)
{
	var t;
	msg_of(t, name);
	speak_msg_d3(t, a, b, c);
}

void rspeak_s(const char *name, value &s)
{
	var t;
	msg_of(t, name);
	speak_msg_s(t, s);
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
		{
			// The reference prints the prompt once at EOF.
			std::print("> ");
			return false;
		}
		string raw;
		getline(cin, raw);
		if ( !cin.good() && raw.length() == 0 )
		{
			std::print("> ");
			return false;
		}
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

// The reference's silent_yes_or_no: read an answer with no question
// speak (the dragon bare-hands prompt already spoke).
bool silent_ask()
{
	for (;;)
	{
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
			return true;
		if ( h == "n" )
			return false;
		rspeak("PLEASE_ANSWER");
	}
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
