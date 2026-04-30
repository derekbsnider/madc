// madc embedded sys/ipc.h — System V IPC constants
// Used with sys/shm.h, sys/msg.h, sys/sem.h

#define IPC_PRIVATE 0

// Creation flags (for shmget/msgget/semget)
#define IPC_CREAT   0x0200
#define IPC_EXCL    0x0400
#define IPC_NOWAIT  0x0800

// Control commands (for shmctl/msgctl/semctl)
#define IPC_RMID 0
#define IPC_SET  1
#define IPC_STAT 2
#define IPC_INFO 3
