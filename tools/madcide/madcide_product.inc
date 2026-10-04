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
// linked in (its shipped plugins load from their bundles).
const ide_product madcide_product = { "madcide", "", ui::TUI, "", 0 };

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
