// madc embedded sys/wait.h — process wait constants
// Functions (wait, waitpid) available via dlsym fallback

#define WNOHANG   1
#define WUNTRACED 2
#define WCONTINUED 8

// Macros to extract status from waitpid() result
// (implemented as simple integer expressions)
#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     (((s) >> 8) & 0xff)
