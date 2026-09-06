#ifndef QF_A_H
#define QF_A_H 1
/* the quoted include below must fall back to the <...> chain: the
   includer-relative candidate is dir/dir/b.h, which does not exist */
#include "dir/b.h"
#define QF_A 40
#endif
