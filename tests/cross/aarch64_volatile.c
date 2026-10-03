/* aarch64_volatile.c — a loop spinning on a volatile sig_atomic_t that a
   signal handler sets (C11 5.1.2.3p5) ends: every volatile access is a real
   load (5.1.2.3p6). MIR had no volatile concept, so -O2's redundant-load
   elimination forwarded the load before the loop into it and the loop never
   ended. The jit leg runs c2m at its default -O2. Declares what it uses: the
   jit leg cannot open the glibc headers. */
int printf (const char *, ...);
typedef int sig_atomic_t;
struct timeval { long tv_sec, tv_usec; };
struct itimerval { struct timeval it_interval, it_value; };
int setitimer (int, const struct itimerval *, struct itimerval *);
void (*signal (int, void (*) (int))) (int);
#define SIGALRM 14
#define ITIMER_REAL 0

static volatile sig_atomic_t by_name;
static volatile struct { int pad; sig_atomic_t flag; } vbox;
static int phase;

static void on_alarm (int sig) {
    (void) sig;
    if (phase == 0) by_name = 1;
    else vbox.flag = 1;
}

static void arm (int p) {
    struct itimerval it = {{0, 0}, {0, 10000}};
    phase = p;
    signal (SIGALRM, on_alarm); /* per alarm: a strict ISO signal() is one-shot (SysV) */
    setitimer (ITIMER_REAL, &it, 0);
}

int main (void) {
    long n0 = 0, n1 = 0;
    arm (0);
    while (!by_name) n0++;
    arm (1);
    while (!vbox.flag) n1++;
    printf ("name: %d\nobject member: %d\n", n0 > 0, n1 > 0);
    return 0;
}
