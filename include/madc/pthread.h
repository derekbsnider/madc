// madc embedded pthread.h — POSIX threads
// On Linux (glibc 2.34+) pthread functions are in libc — dlsym fallback works.
// On older systems, use: #load "libpthread.so.0" as pthread;
// Functions: pthread_create, pthread_join, pthread_detach, pthread_self,
//            pthread_exit, pthread_cancel, pthread_equal,
//            pthread_mutex_init, pthread_mutex_lock, pthread_mutex_trylock,
//            pthread_mutex_unlock, pthread_mutex_destroy,
//            pthread_cond_init, pthread_cond_wait, pthread_cond_signal,
//            pthread_cond_broadcast, pthread_cond_destroy,
//            pthread_attr_init, pthread_attr_destroy,
//            pthread_attr_setdetachstate, pthread_attr_getstacksize,
//            pthread_rwlock_init, pthread_rwlock_rdlock,
//            pthread_rwlock_wrlock, pthread_rwlock_unlock,
//            pthread_rwlock_destroy
// Opaque POSIX thread types. The implementations live in libc (dlsym fallback),
// but the TYPES must be declared so consumers can `typedef pthread_mutex_t X;`,
// declare `pthread_mutex_t *` parameters, and size storage — without this,
// libstdc++'s <bits/gthr-default.h> (`typedef pthread_mutex_t __gthread_mutex_t;`
// …) fails with "Expecting type after 'typedef'". Sizes/alignment match glibc's
// x86-64 layout (a union of a byte array + an alignment member), so by-value
// storage and sizeof are correct; the contents stay opaque (manipulated only by
// the libc pthread functions).
typedef union { char __size[40]; long __align; } pthread_mutex_t;
typedef union { char __size[48]; long long __align; } pthread_cond_t;
typedef union { char __size[56]; long __align; } pthread_rwlock_t;
typedef union { char __size[56]; long __align; } pthread_attr_t;
typedef union { char __size[32]; long __align; } pthread_barrier_t;
typedef union { char __size[4];  int __align; } pthread_mutexattr_t;
typedef union { char __size[4];  int __align; } pthread_condattr_t;
typedef union { char __size[8];  long __align; } pthread_rwlockattr_t;
typedef union { char __size[4];  int __align; } pthread_barrierattr_t;
typedef unsigned int pthread_key_t;
typedef int pthread_once_t;
typedef volatile int pthread_spinlock_t;

// Thread creation attributes
#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1

// Mutex type attributes
#define PTHREAD_MUTEX_TIMED_NP     0
#define PTHREAD_MUTEX_DEFAULT      0
#define PTHREAD_MUTEX_RECURSIVE    1
#define PTHREAD_MUTEX_ERRORCHECK   2
#define PTHREAD_MUTEX_ADAPTIVE_NP  3

// Process sharing
#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED  1

// Cancellation
#define PTHREAD_CANCEL_ENABLE   0
#define PTHREAD_CANCEL_DISABLE  1
#define PTHREAD_CANCEL_DEFERRED 0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED ((int64_t)-1)

// Scope
#define PTHREAD_SCOPE_SYSTEM  0
#define PTHREAD_SCOPE_PROCESS 1

// pthread_t type alias
#define pthread_t int64_t
