// The dialect builtin registry's table in STAGE ONE of the build
// (docs/plans/2026-10-17-dialect-builtin-registration.md): no entries. The
// stage-one madc writes the real table (`--emit-builtin-registry=`) into the
// object tree, and stage two links that table in place of this one.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <iostream>
#include <sstream>
#include <memory>
#include <stdint.h>


#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_builtin_registry.h"

namespace madc { namespace builtins {
const MadcBuiltinEntry dialect_table[] = { { "", "", NULL } };
const size_t dialect_table_count = 0;
const MadcBuiltinBareName dialect_bare_names[] = { { "", "", "" } };
const size_t dialect_bare_names_count = 0;
const MadcFragmentWord dialect_fragment_words[] = { { "", "", false } };
const size_t dialect_fragment_words_count = 0;
} }
