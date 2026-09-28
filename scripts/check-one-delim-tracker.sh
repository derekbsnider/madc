#!/bin/bash
# RATCHET GATE — one shared balanced-delimiter tracker.
#
# The rule: `(` `[` `{` `<` nesting bookkeeping in a token or spelling scan
# belongs to DelimDepth (src/parser.cpp) and its two step helpers
# (delim_scan_step / Program::delimStepStream). NOTHING else may hand-roll it.
#
# Why a gate and not a comment: DelimDepth's own header comment has said
# "this is the single shared bookkeeping" since it was written, and eleven
# hand-rolled copies were added anyway — a comment only reaches someone
# already reading that function, which is exactly not the person writing the
# next copy. The angle-bracket disambiguation bug (a `<` counted as a template
# open inside `decltype(a < b)`, consuming a whole header and leaving its
# namespace unclosed) lived for seven weeks because the fix landed in one copy.
#
# Marker note: this counts the CONCEPT (a local delimiter-depth counter), not
# one spelling of it. Three earlier audits undercounted this family by grepping
# `++angle_depth`, which missed a counter named plain `depth`, missed
# DelimDepth's own `++angle`, and missed every paren/square/brace-only scanner.
#
# ...and then THIS GATE made the same mistake, which is worse, because a green
# gate stops you looking. Its first marker was `int (angle|paren|square|brace)_depth`
# — a SPELLING. It reported "GREEN, 0 trackers" on 2026-07-27 while eleven
# hand-rolled scanners named plain `angle` / `paren` / `square` sat in
# parser.cpp, one of them (22581) the very unguarded `++angle` on every tkLT
# that this whole campaign exists to eliminate.
#
# The marker now combines the name check with a behavior check: a character
# comparison against `(` followed by incrementing a counter and a matching `)`
# decrement of that SAME counter.  The back-reference catches plain `depth`,
# `pdepth`, `mdepth`, or whatever the next copy is called.  If you find yourself
# narrowing either half to make the count go down, you are doing the thing this
# comment is about.
#
# Ratchet: the count must never rise. Target is 0. Lower BASELINE whenever a
# scanner is migrated; never raise it.
set -u
cd "$(dirname "$0")/.."

if ! command -v perl >/dev/null 2>&1; then
	echo "REGRESSION — perl is required for the name-independent delimiter gate."
	exit 1
fi

# 74 on 2026-09-28, when the token marker below first saw the token scans
# (BUGS.md B58-B61). Each migration lowers it.
BASELINE=72

# A hand-rolled tracker always declares at least one delimiter-depth local.
#
# EXCLUDED, deliberately — validate_expression_source() in madc_program.cpp is a
# raw-SOURCE mini-lexer with quote and comment states, not token-delimiter
# bookkeeping. It also tracks whether each paren level was preceded by an
# identifier, to allow commas in call args but not in grouping parens. The
# tie-breaker ("would a change to the delimiter rule require editing it?") says
# no: it would evolve with expression-validation, not with [temp.names].
# Merging it into DelimDepth would be worse than the duplication.
# EXCLUDED, deliberately: the two shared trackers' OWN member declarations.
#   src/parser.cpp          — DelimDepth (token level)
#   include/spelling_delim.h — SpellingDelimDepth (char level, shared header)
hits=$(grep -rnE '\bint +[a-z_]*(angle|paren|square|brace)[a-z_]* *= *0|, *[a-z_]*(angle|paren|square|brace)[a-z_]* *= *0' \
    src/ include/ \
  | grep -vE '^include/doctest\.h:' \
  | grep -vE '^src/madc_program\.cpp:' \
  | grep -vE '^include/spelling_delim\.h:' \
  | grep -vE '^src/parser\.cpp:[0-9]+:    int paren = 0, square = 0, brace = 0, angle = 0;$' \
  | grep -vE 'size_t' )        # `size_t lparen = 0` is an INDEX, not a counter

# Name-independent raw-spelling form.  Match the open and close arms together,
# with the counter captured and back-referenced, so an unrelated `++pos` after
# seeing an opening parenthesis is not classified as nesting bookkeeping.
behavior_hits=$(find src include -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
  | xargs -0 perl -0777 -ne '
      next if $ARGV eq "src/madc_program.cpp"
           || $ARGV eq "include/spelling_delim.h";
      while (/(?:if|else if)\s*\(\s*[^\n]+?==\s*\x27\(\x27\s*\)\s*(?:\{\s*)?\+\+([A-Za-z_][A-Za-z0-9_]*).{0,500}?(?:if|else if)\s*\(\s*[^\n]+?==\s*\x27\)\x27.{0,100}?--\1/sg) {
          my $line = 1 + (substr($_, 0, $-[0]) =~ tr/\n/\n/);
          print "$ARGV:$line:raw balanced-parenthesis counter\n";
      }' )

if [ -n "$behavior_hits" ]; then
	hits="${hits}${hits:+$'\n'}${behavior_hits}"
fi

# Name-independent TOKEN form, all four delimiters.  ...and the two markers
# above still had a hole, the one this whole comment is about: the behavior
# check matched only a raw `'('` CHARACTER scan, so a TOKEN scan
# (`id() == TokenID::tkLT` ... `++depth`) with a counter named anything but
# angle/paren/square/brace was invisible. The gate reported "GREEN, 0" on
# 2026-09-28 over 73 of them (BUGS.md B58-B61). This marker matches the
# behavior on tokens and characters alike: an equality test on an OPEN
# delimiter (or on a bare variable, the `tsubst_matching_close(v, i, open_id,
# close_id)` shape) that increments a counter, then a test on a CLOSE
# delimiter (or a variable) that decrements the SAME counter. DelimDepth's own
# body is the owner and is skipped.
token_marker='
  next if $ARGV =~ m{(^|/)(madc_program\.cpp|spelling_delim\.h|doctest\.h|json\.hpp)$};
  my ($os, $oe) = (-1, -1);
  if (/^struct DelimDepth \{.*?^\};/ms) { ($os, $oe) = ($-[0], $+[0]); }
  my $open  = qr/TokenID::tk(?:LT|OpBrk|OpSqr|OpBrc)\b|\x27[(\[{<]\x27|"[(\[{<]"/;
  my $close = qr/TokenID::tk(?:GT|BSR|ClBrk|ClSqr|ClBrc)\b|\x27[)\]}>]\x27|">>?"|"[)\]}]"/;
  my $var   = qr/==\s*[A-Za-z_]\w*\s*\)/;
  # Either delimiter on either side: a BACKWARD walk counts `>` up and `<`
  # down, and is the same tracker.
  my $test  = qr/(?:$open|$close|$var)/;
  my %seen;
  while (/$test[^;]{0,60}?(?:\+\+\s*([A-Za-z_]\w*)|\b([A-Za-z_]\w*)\s*(?:\+\+|\+=))/sg) {
      my $nm = defined $1 ? $1 : $2;
      my ($at, $end) = ($-[0], $+[0]);
      next if $at >= $os && $at < $oe;
      next unless substr($_, $end, 1500) =~ /$test[^;]{0,60}?(?:--\s*\Q$nm\E\b|\b\Q$nm\E\s*(?:--|-=))/s;
      my $line = 1 + (substr($_, 0, $at) =~ tr/\n/\n/);
      print "$ARGV:$line:token balanced-delimiter counter $nm\n" unless $seen{$line}++;
  }'

# Negative control: three hand-rolled token counters (one named nothing like a
# delimiter, one over variable ids, one walking backwards) must be caught;
# DelimDepth's own update() and a non-delimiter nesting counter (`?` / `:`)
# must not.
ctl_dir=$(mktemp -d)
trap 'rm -rf "$ctl_dir"' EXIT
cat > "$ctl_dir/ctl.cpp" <<'CTL'
struct DelimDepth {
    void update(TokenBase *t)
    {
	switch ( t->id() )
	{
	    case TokenID::tkOpBrk: ++paren; break;
	    case TokenID::tkClBrk: if ( paren > 0 )  --paren;  break;
	}
    }
};
static size_t arity(const std::vector<TokenBase *> &decl)
{
    int q = 0;
    for ( size_t i = 0; i < decl.size(); ++i )
    {
	if ( decl[i]->id() == TokenID::tkLT ) ++q;
	else if ( decl[i]->id() == TokenID::tkGT ) --q;
    }
    return q;
}
static size_t matching(const std::vector<TokenBase *> &v, TokenID open_id, TokenID close_id)
{
    int k = 0;
    for ( size_t i = 0; i < v.size(); ++i )
    {
	if ( v[i]->id() == open_id ) ++k;
	else if ( v[i]->id() == close_id ) { if ( --k == 0 ) return i; }
    }
    return v.size();
}
static size_t backward(const std::vector<TokenBase *> &v, size_t k)
{
    int b = 0;
    for ( ;; --k )
    {
	if ( v[k]->id() == TokenID::tkGT ) ++b;
	else if ( v[k]->id() == TokenID::tkLT ) { if ( --b == 0 ) return k; }
    }
}
static int ternaries(const std::vector<TokenBase *> &v)
{
    int nested = 0;
    for ( TokenBase *t : v )
    {
	if ( t->id() == TokenID::tkQmark ) ++nested;
	else if ( t->id() == TokenID::tkColon ) --nested;
    }
    return nested;
}
CTL
ctl=$(perl -0777 -ne "$token_marker" "$ctl_dir/ctl.cpp" | grep -c .)
if [ "$ctl" -ne 3 ]; then
	echo "check-one-delim-tracker: NEGATIVE CONTROL FAILED -- the token marker matched"
	echo "  $ctl of the 3 planted counters (DelimDepth and the ?: counter must not match)"
	exit 1
fi

token_hits=$(find src include -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
  | xargs -0 perl -0777 -ne "$token_marker")
if [ -n "$token_hits" ]; then
	hits="${hits}${hits:+$'\n'}${token_hits}"
fi
hits=$(printf '%s\n' "$hits" | grep . | sort -t: -k1,1 -k2,2n -u)
n=$(printf '%s' "$hits" | grep -c . )

# --- the Program handle on every STREAM scan ---------------------------------
# DelimDepth decides whether a `<` opens a template-argument-list by NAME
# LOOKUP ([temp.names]/3 — DelimDepth::lt_reads_as_less_than, the reading gcc's
# cp_parser_template_name and clang's Sema::isTemplateName give), and lookup
# needs the Program. A scan that walks the live token stream (nextToken /
# peekToken in the same function) sits at a parse position where lookup is
# valid, so a tracker it drives itself (a direct `x.update(...)`) must be
# constructed with the handle: `DelimDepth x(this)` in a Program member,
# `DelimDepth x(&pgm)` in a static helper. A bare `DelimDepth x;` driven
# directly in such a function is a copy of the token-only rule — the shape the
# 2026-09-04 base-clause splitter bug lived in: the dependent-name rule landed
# in delimStepStream, the argument splitter built its own tracker, called
# update() itself and never saw it. Trackers fed only through delim_scan_step
# (index scans over stored token runs) or delimStepStream (which sets the
# handle) are not this shape.
stream_hits=$(perl -e '
  open(my $fh, "<", "src/parser.cpp") or die;
  my @lines = <$fh>; my ($in, %bare, %direct, $stream, $head) = (0);
  sub flush {
    if ($stream) {
      for my $nm (sort keys %bare) {
        print "src/parser.cpp:$bare{$nm}: bare DelimDepth $nm driven directly in a stream scan\n" if $direct{$nm};
      }
    }
    %bare = (); %direct = (); $stream = 0;
  }
  for my $i (0..$#lines) {
    my $l = $lines[$i];
    if ($l =~ /^[A-Za-z_].*\(/ && $l !~ /;\s*$/) { flush(); $in = 1; $head = $l; }
    next unless $in;
    $bare{$1} = $i + 1 if $l =~ /^\s*DelimDepth\s+([A-Za-z_]\w*)\s*;/;
    for my $nm (keys %bare) { $direct{$nm} = 1 if $l =~ /\b$nm\.update\s*\(/; }
    $stream = 1 if $l =~ /\b(nextToken|peekToken)\s*\(/;
    if ($l =~ /^\}/) { flush(); $in = 0; }
  }')
if [ -n "$stream_hits" ]; then
	echo "REGRESSION — a stream scan drives a DelimDepth it built WITHOUT the Program handle."
	echo "Construct it with the Program (DelimDepth d(this) / DelimDepth d(&pgm)) so the"
	echo "[temp.names]/3 lookup reading of '<' reaches it. See .claude/rules/delimiter-tracking.md"
	printf '%s\n' "$stream_hits" | sed 's/^/  /'
	exit 1
fi

echo "one-delim-tracker ratchet: $n hand-rolled delimiter-depth locals (baseline $BASELINE, target 0)"

if [ "$n" -gt "$BASELINE" ]; then
	echo "REGRESSION — a new hand-rolled delimiter tracker was added."
	echo "Use DelimDepth + delim_scan_step()/delimStepStream() instead."
	echo "See .claude/rules/delimiter-tracking.md"
	printf '%s\n' "$hits" | sed 's/^/  /'
	exit 1
fi

if [ "$n" -lt "$BASELINE" ]; then
	echo "RATCHET FORWARD — $((BASELINE - n)) scanner(s) migrated since the baseline."
	echo "Lower BASELINE in $0 to $n to lock the gain in."
	exit 1
fi

if [ "$n" -eq 0 ]; then
	echo "GREEN — DelimDepth is the only delimiter tracker."
	exit 0
fi

echo "held at baseline — $n scanner(s) still to migrate (see the rule file)."
exit 0
