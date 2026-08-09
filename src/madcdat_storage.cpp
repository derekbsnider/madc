#include "madcdis/driver.h"

namespace madc {

#ifdef HAVE_BDB
void register_bdb_storage_driver(DataDriverRegistry &registry);
#endif
#ifdef HAVE_GDBM
void register_gdbm_storage_driver(DataDriverRegistry &registry);
#endif
#ifdef HAVE_QDBM
void register_qdbm_storage_driver(DataDriverRegistry &registry);
#endif
#ifdef HAVE_SQLITE3
void register_sqlite_storage_driver(DataDriverRegistry &registry);
#endif

void register_optional_storage_drivers(DataDriverRegistry &registry)
{
#ifdef HAVE_BDB
	register_bdb_storage_driver(registry);
#endif
#ifdef HAVE_GDBM
	register_gdbm_storage_driver(registry);
#endif
#ifdef HAVE_QDBM
	register_qdbm_storage_driver(registry);
#endif
#ifdef HAVE_SQLITE3
	register_sqlite_storage_driver(registry);
#endif
}

} // namespace madc
