// madc embedded sys/shm.h — shared memory IPC
// Functions (shmget, shmat, shmdt, shmctl) available via dlsym fallback
// struct shmid_ds access deferred

// shmat() flags
#define SHM_RDONLY  0x01000
#define SHM_RND    0x02000
#define SHM_REMAP  0x04000
#define SHM_EXEC   0x08000

// shmctl() commands (in addition to IPC_* from sys/ipc.h)
#define SHM_LOCK   11
#define SHM_UNLOCK 12
#define SHM_STAT   13
#define SHM_INFO   14
