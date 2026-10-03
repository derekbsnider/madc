/* Every access through a volatile lvalue is performed (C11 5.1.2.3p6), so a loop spinning on a
   volatile sig_atomic_t that a signal handler sets (5.1.2.3p5) ends at every optimization level:
   the generator must not hoist, merge or forward its loads.  The object is reached four ways: by
   name, through a pointer to volatile, as a volatile member, and as a member of a volatile
   object.  */
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>

static volatile sig_atomic_t by_name, by_pointer;
static struct { int pad; volatile sig_atomic_t flag; } box;
static volatile struct { int pad; sig_atomic_t flag; } vbox;
static int phase;

static void on_alarm (int sig) {
  (void) sig;
  switch (phase) {
  case 0: by_name = 1; break;
  case 1: by_pointer = 1; break;
  case 2: box.flag = 1; break;
  default: vbox.flag = 1; break;
  }
}

static void arm (int p) {
  struct itimerval it = {{0, 0}, {0, 10000}};
  phase = p;
  signal (SIGALRM, on_alarm); /* per alarm: a strict ISO signal() is one-shot (SysV) */
  setitimer (ITIMER_REAL, &it, 0);
}

int main (void) {
  volatile sig_atomic_t *p = &by_pointer;
  long n0 = 0, n1 = 0, n2 = 0, n3 = 0;

  arm (0);
  while (!by_name) n0++;
  arm (1);
  while (!*p) n1++;
  arm (2);
  while (!box.flag) n2++;
  arm (3);
  while (!vbox.flag) n3++;
  printf ("name: %d\npointer: %d\nmember: %d\nobject member: %d\n", n0 > 0, n1 > 0, n2 > 0, n3 > 0);
  return 0;
}
