// madcide_product.inc — the PRODUCT in use (<madcide/product>; plugin design
// §9.3): madcide's own descriptor, and the one record of the descriptor
// madcide_main was given. Every reader of the product's name, bundle, face or
// linked plugin asks product_in_use(); a unit that includes the base's layers
// without madcide_main (a test) reads madcide's own.
//
// Thread contract: product_use is the one write, by madcide_main before any
// session or thread starts; every read after it is of an unchanging record.

#include "include/madcide/product"

// madcide: the console editor by default, the default bundle, no plugin
// linked in (its shipped plugins load from their bundles). It has no version
// of its own: it is madc's, which About shows.
const ide_product madcide_product = { "madcide", "", ui::TUI, "", 0, "madcide", 0,
				      "https://github.com/derekbsnider/madc",
				      "Derek Snider", MADCIDE_MPL2_SUMMARY };

const ide_product *product_given = 0;	// madcide_main's; 0 = madcide's own

void product_use(const ide_product &p)
{
    product_given = &p;
}

const ide_product &product_in_use()
{
    if ( product_given == 0 )
	return madcide_product;
    return *product_given;
}

// The product's name as shown (the window title, About): its title, else
// its name.
const char *product_title()
{
    const ide_product &p = product_in_use();
    if ( p.title == 0 || p.title[0] == 0 )
	return p.name;
    return p.title;
}

// Help ▸ About's lines (Thonny's About): the product and its own version,
// its home page, the madc it runs on, the copyright, the licence summary —
// a blank line between the parts, a part the descriptor leaves out (0)
// omitted. The year is the build's (__DATE__ ends in it); madc's version is
// the running engine's (madc::sys.version).
void product_about(var &rows)
{
    const ide_product &p = product_in_use();
    rows = {};
    if ( p.version != 0 && p.version[0] != 0 )
	rows.push(format("{} {}", product_title(), p.version));
    else
	rows.push(product_title());
    if ( p.url != 0 && p.url[0] != 0 )
    {
	rows.push("");
	rows.push(p.url);
    }
    rows.push("");
    rows.push(format("MadC {}", madc::sys.version));
    if ( p.copyright != 0 && p.copyright[0] != 0 )
    {
	const char *date = __DATE__;	// "Mmm dd yyyy"
	rows.push("");
	rows.push(format("Copyright (c) {} {}", date + strlen(date) - 4, p.copyright));
    }
    if ( p.license != 0 && p.license[0] != 0 )
    {
	rows.push("");
	array lines;
	php::explode(lines, "\n", p.license);
	for ( var line : lines )
	    rows.push(line);
    }
}
