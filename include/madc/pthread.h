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
// struct pthread_mutex_t / pthread_cond_t / pthread_attr_t deferred

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
