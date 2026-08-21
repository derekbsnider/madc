// ---- RNG: the reference's deterministic LCG (advent.h) ----------------
// A=1093, C=221587, M=1048576. A transcript's `seed N` pins the stream;
// dwarf walks and pct rolls then replay exactly. State is a script
// global for the single session (A10 banks it into world-state).

long LCG_X = 0;

long randrange(long range)
{
	long old = LCG_X;
	LCG_X = (1093 * LCG_X + 221587) % 1048576;
	return range * old / 1048576;
}

bool pct(long n)
{
	return randrange(100) < n;
}

void rng_seed(long seedval)
{
	LCG_X = seedval % 1048576;
	if ( LCG_X < 0 )
		LCG_X = 1048576 + LCG_X;
	// The reservoir magic word regenerates with the seed: five draws
	// (they ADVANCE the stream — part of RNG parity), then the second
	// character is forced to an apostrophe.
	var alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	var zz;
	zz = "";
	long i = 0;
	while ( i < 5 )
	{
		zz += alpha.substr(randrange(26), 1);
		i++;
	}
	var rebuilt = zz.substr(0, 1);
	rebuilt += "'";
	rebuilt += zz.substr(2, 3);
	ZZWORD = rebuilt;
}
