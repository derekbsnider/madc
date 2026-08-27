// SPDX-License-Identifier: MPL-2.0
// taskio — the cooperative scheduler's io-wait publics (MT-4b), implemented
// beside the select discipline they share in src/madc_task_chan.cpp. The
// scheduler itself stays fd-blind: this layer owns the waiter registry and
// installs __madc_task_io_wait_hook (rt_task.h) on first use.
//
// Handles are CRT fds on every platform (ProcessPipeChannel's contract —
// Windows process pipes are _open_osfhandle-converted). "Readable" means a
// read() would make progress NOW: data available, EOF, or an error the read
// will surface.
//
// THREAD-SAFETY CONTRACT (thread-safety.md): scheduler-thread only, like
// every task verb — callers on the single cooperative OS thread. Process
// pump helper threads must never enter here.

#ifndef __MADC_TASK_IO_H
#define __MADC_TASK_IO_H 1

#include <stdint.h>

namespace madc {
namespace taskio {

// Zero-timeout readability probe. Never parks.
bool poll_readable(intptr_t handle);

// Park the current task until `handle` is readable (the scheduler's io-wait
// seat wakes it). Returns immediately when it is readable already. Works
// with no other task live: the park routes through task_next_or_wait, whose
// io wait blocks in poll() for us.
void wait_readable(intptr_t handle);

} // namespace taskio
} // namespace madc

#endif // __MADC_TASK_IO_H
