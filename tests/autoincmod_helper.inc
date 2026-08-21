// Helper module for testautoincmodule.mad — deliberately ZERO includes
// and ZERO std:: qualification: the auto-include scan must serve
// identifiers inside a QUOTED user module exactly as it serves the main
// file (bare print/println/format intrinsics, php::, the value carrier),
// and the prelude must insert BEFORE this module's tokens.
void shout(value &out, value &word)
{
	var up = php::strtoupper(word);
	out = format("{}!", up.c_str()).c_str();
	println("module says {}", out);
}

bool probe_paths()
{
	// php::file_exists — PHP parity: a DIRECTORY counts as existing.
	if ( !php::file_exists("tests") )
		return false;
	return !php::file_exists("tests/no_such_file_madc_xyz");
}
