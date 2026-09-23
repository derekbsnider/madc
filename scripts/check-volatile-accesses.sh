#!/bin/bash
# BEHAVIOUR GATE -- every access through a volatile lvalue is performed exactly
# as written (C11 5.1.2.3p6): the generator never removes one, merges two,
# hoists one out of a loop, forwards a value to or from one, or folds one into
# a read-modify-write insn. MIR carries the qualifier as MIR_mem_t.volatile_p,
# set by c2mir and honoured by every MIR-gen pass through volatile_mem_insn_p.
#
# The count is MEASURED, not read from a dump: the cases run with their
# volatile objects on a PROT_NONE page. Each access faults; the SIGSEGV handler
# counts it, opens the page and sets the trap flag; the SIGTRAP after that one
# insn closes the page again. So a count is the number of memory-accessing
# insns that touched the objects -- a read-modify-write insn counts once, a
# load plus a store twice.
#
# Oracle: gcc -O2 compiling the same cases. c2m at -O0..-O3 must match it case
# for case. Two-sided: the control compiles the cases with -Dvolatile= and
# must count FEWER accesses (the passes are live and the counter is not blind).
# madc joins when its front end carries a pointee volatile (KG pointee_volatile);
# today only a volatile OBJECT reaches the IR.
#
# Two ONE-OWNER rules keep it that way (static, every host, two-sided):
#   - c2mir builds the operand of a sub-object (a member, an __int128 half, a
#     complex component, a block chunk) only through mem_part_op, which carries
#     the object's volatile bit to its parts. A MIR_new_*mem_op call whose
#     displacement is read off another operand's u.mem.disp is a copy of it.
#   - MIR-gen passes ask volatile_mem_insn_p; a volatile_p read anywhere else
#     in mir-gen.c is a second, divergent test.
#
# The fault/trap counter is x86-64 Linux; elsewhere that half says so.
set -u
cd "$(dirname "$0")/.."

# sub_object_copies FILE... : a MIR_new_mem_op / MIR_new_alias_mem_op call whose
# arguments read another operand's u.mem.disp, outside mem_part_op itself.
sub_object_copies() {
	awk '
		/^static [^(]* mem_part_op \(/ { owner = 1 }
		owner && /^}/ { owner = 0; next }
		owner { next }
		!call && /MIR_new_(alias_)?mem_op \(/ { call = 1; at = FILENAME ":" FNR; text = "" }
		call { text = text $0; if ($0 ~ /;/) { if (text ~ /u\.mem\.disp/) print at; call = 0 } }
	' "$@"
}
# stray_volatile_tests FILE... : a volatile_p read outside volatile_mem_insn_p
# (comments skipped).
stray_volatile_tests() {
	awk '
		{ line = $0
		  if (incom) { if (sub(/.*\*\//, "", line)) incom = 0; else next }
		  gsub(/\/\*([^*]|\*+[^*\/])*\*+\//, "", line)
		  if (match(line, /\/\*/)) { line = substr(line, 1, RSTART - 1); incom = 1 }
		}
		/^static int volatile_mem_insn_p \(/ { owner = 1 }
		owner && /^}/ { owner = 0; next }
		!owner && line ~ /volatile_p/ { print FILENAME ":" FNR ": " $0 }
	' "$@"
}

static_fail=0
ctl=$(mktemp -d)
cat > "$ctl/bad_part.c" <<'CTL'
static MIR_op_t mem_part_op (MIR_context_t ctx, MIR_op_t mem, MIR_type_t type, MIR_disp_t offset) {
  return MIR_new_mem_op (ctx, type, mem.u.mem.disp + offset, 0, 0, 1);
}
static void f (void) {
  x = MIR_new_mem_op (ctx, t,
                      mem.mir_op.u.mem.disp + 8, b, 0, 1);
}
CTL
cat > "$ctl/good_part.c" <<'CTL'
static MIR_op_t mem_part_op (MIR_context_t ctx, MIR_op_t mem, MIR_type_t type, MIR_disp_t offset) {
  return MIR_new_mem_op (ctx, type, mem.u.mem.disp + offset, 0, 0, 1);
}
static void f (void) { x = mem_part_op (ctx, mem.mir_op, t, 8); y = MIR_new_mem_op (ctx, t, 0, r, 0, 1); }
CTL
cat > "$ctl/bad_gen.c" <<'CTL'
static int volatile_mem_insn_p (MIR_insn_t insn) {
  return insn->ops[0].u.var_mem.volatile_p;
}
static void dse (void) { if (insn->ops[0].u.var_mem.volatile_p) return; }
CTL
cat > "$ctl/good_gen.c" <<'CTL'
/* the flag is MIR_mem_t.volatile_p */
static int volatile_mem_insn_p (MIR_insn_t insn) {
  return insn->ops[0].u.var_mem.volatile_p;
}
static void dse (void) { if (volatile_mem_insn_p (insn)) return; } /* not volatile_p directly */
CTL
if [ -z "$(sub_object_copies "$ctl/bad_part.c")" ] || [ -n "$(sub_object_copies "$ctl/good_part.c")" ] \
   || [ -z "$(stray_volatile_tests "$ctl/bad_gen.c")" ] || [ -n "$(stray_volatile_tests "$ctl/good_gen.c")" ]; then
	echo "RED  negative control: the one-owner scans no longer tell a copy from the owner" >&2
	static_fail=1
fi
rm -rf "$ctl"
copies=$(sub_object_copies third_party/mir/c2mir/c2mir.c)
if [ -n "$copies" ]; then
	echo "RED  a sub-object operand built by hand (use mem_part_op, which keeps the volatile bit):" >&2
	echo "$copies" | sed 's/^/       /' >&2
	static_fail=1
fi
stray=$(stray_volatile_tests third_party/mir/mir-gen.c)
if [ -n "$stray" ]; then
	echo "RED  a volatile test outside volatile_mem_insn_p (ask the one predicate):" >&2
	echo "$stray" | sed 's/^/       /' >&2
	static_fail=1
fi
[ "$static_fail" -eq 0 ] || exit 1

C2M=${C2M:-obj/mir/host/c2m}
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
	echo "check-volatile-accesses: one-owner rules green; the access count is skipped (x86-64 Linux only)"
	exit 0
fi
if ! command -v gcc > /dev/null || [ ! -x "$C2M" ]; then
	echo "check-volatile-accesses: needs gcc and $C2M (make -C src)" >&2
	exit 1
fi

w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT

cat > "$w/cases.c" <<'CASES'
struct S { volatile int v; int w; };
struct T { int a, b; };
struct B { volatile unsigned f : 3, g : 5; };

void c_touch (volatile int *vp, int *np, struct S *s) {
  *vp = 1;           /* dead-store elimination keeps both */
  *vp = 2;
  (void) *vp;        /* a discarded read is still a read */
  *vp;
  (void) (*vp, 0);   /* so is a comma's left operand */
  int a = *vp, b = *vp; /* two reads, never merged */
  *np = a + b;
  *np = 7;           /* not volatile: the first store is dead */
  s->v += 3;         /* a load and a store, never one read-modify-write insn */
}
int c_loop (volatile int *vp, int n) {
  int t = 0;
  for (int i = 0; i < n; i++) t += *vp; /* never hoisted out of the loop */
  return t;
}
void c_field (volatile struct T *t) { t->a = 1; t->b = t->a; }
void c_bits (struct B *b) { b->f = 5; }
void c_complex (volatile _Complex double *z) { *z += 1.0; }
void c_index (volatile int *vp) { vp[2] = vp[1]; }
CASES

cat > "$w/harness.c" <<'HARNESS'
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

struct S { volatile int v; int w; };
struct T { int a, b; };
struct B { volatile unsigned f : 3, g : 5; };
void c_touch (volatile int *, int *, struct S *);
int c_loop (volatile int *, int);
void c_field (volatile struct T *);
void c_bits (struct B *);
void c_complex (volatile _Complex double *);
void c_index (volatile int *);

static char *page;
static long page_size, hits;

static void on_segv (int sig, siginfo_t *si, void *arg) {
  ucontext_t *uc = arg;
  (void) sig;
  if ((char *) si->si_addr < page || (char *) si->si_addr >= page + page_size) abort ();
  hits++;
  mprotect (page, page_size, PROT_READ | PROT_WRITE);
  uc->uc_mcontext.gregs[REG_EFL] |= 0x100; /* trap after this one insn */
}
static void on_trap (int sig, siginfo_t *si, void *arg) {
  ucontext_t *uc = arg;
  (void) sig, (void) si;
  mprotect (page, page_size, PROT_NONE);
  uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
}
static void watch (void) { hits = 0; mprotect (page, page_size, PROT_NONE); }
static long done (void) { mprotect (page, page_size, PROT_READ | PROT_WRITE); return hits; }

int main (void) {
  struct sigaction sa = { 0 };
  int n;
  page_size = sysconf (_SC_PAGESIZE);
  page = mmap (0, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  sa.sa_flags = SA_SIGINFO;
  sa.sa_sigaction = on_segv;
  sigaction (SIGSEGV, &sa, 0);
  sa.sa_sigaction = on_trap;
  sigaction (SIGTRAP, &sa, 0);
  watch (); c_touch ((volatile int *) page, &n, (struct S *) (page + 64)); printf ("touch %ld\n", done ());
  watch (); c_loop ((volatile int *) page, 5); printf ("loop %ld\n", done ());
  watch (); c_field ((volatile struct T *) (page + 128)); printf ("field %ld\n", done ());
  watch (); c_bits ((struct B *) (page + 192)); printf ("bits %ld\n", done ());
  watch (); c_complex ((volatile _Complex double *) (page + 256)); printf ("complex %ld\n", done ());
  watch (); c_index ((volatile int *) (page + 320)); printf ("index %ld\n", done ());
  return 0;
}
HARNESS

# count OBJ : link the cases object against the harness and print its counts.
count() {
	gcc -O0 -o "$w/run" "$w/harness.c" "$1" 2> "$w/link.err" || { cat "$w/link.err" >&2; return 1; }
	( ulimit -t 20; timeout 20 "$w/run" )
}

fail=0
gcc -O2 -c -o "$w/gcc.o" "$w/cases.c" || exit 1
oracle=$(count "$w/gcc.o") || { echo "check-volatile-accesses: the gcc oracle did not run" >&2; exit 1; }
want="touch 9
loop 5
field 3
bits 2
complex 4
index 2"
if [ "$oracle" != "$want" ]; then
	echo "RED  the harness no longer measures what it did: gcc -O2 counts" >&2
	echo "$oracle" | sed 's/^/       /' >&2
	fail=1
fi

for lvl in -O0 -O1 -O2 -O3; do
	rm -f "$w/c2m.o"
	"$C2M" $lvl -fobject "$w/cases.c" -o "$w/c2m.o" || { echo "RED  c2m $lvl did not compile the cases" >&2; fail=1; continue; }
	got=$(count "$w/c2m.o") || { echo "RED  c2m $lvl: the cases did not run" >&2; fail=1; continue; }
	if [ "$got" != "$oracle" ]; then
		echo "RED  c2m $lvl performs a different number of volatile accesses than gcc -O2:" >&2
		diff <(echo "$oracle") <(echo "$got") | sed 's/^/       /' >&2
		fail=1
	fi
done

# Negative control: without the qualifier the same passes must remove accesses.
rm -f "$w/ctl.o"
"$C2M" -O2 -Dvolatile= -fobject "$w/cases.c" -o "$w/ctl.o" || exit 1
ctl=$(count "$w/ctl.o") || { echo "check-volatile-accesses: the control did not run" >&2; exit 1; }
sum() { awk '{ s += $2 } END { print s + 0 }'; }
if [ "$(echo "$ctl" | sum)" -ge "$(echo "$oracle" | sum)" ]; then
	echo "RED  negative control: c2m -O2 -Dvolatile= performed as many accesses as the volatile build" >&2
	echo "$ctl" | sed 's/^/       /' >&2
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "check-volatile-accesses: c2m -O0..-O3 perform every volatile access gcc -O2 does ($(echo "$oracle" | sum) across six cases); the non-volatile control performs $(echo "$ctl" | sum)"
exit 0
