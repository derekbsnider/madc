#ifndef __LIBMADCDAT_DRIVER_SHIM_H
#define __LIBMADCDAT_DRIVER_SHIM_H 1

// TRANSITIONAL forwarding shim (task #58 / plan C1): the driver interface
// header moved to its permanent home in include/madcdis/ — the madc::dis
// core substrate surface. This shim keeps the old include path working for
// out-of-tree consumers during the transition; in-tree code includes
// "madcdis/driver.h" directly. Deletion horizon: the first release after
// external drivers (libmadcdat) ship against the madcdis paths.

#include "madcdis/driver.h"

#endif // __LIBMADCDAT_DRIVER_SHIM_H
