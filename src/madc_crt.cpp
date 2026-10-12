// madc_crt.cpp — the startup members madc links into the images it makes.
//
// A gcc-linked image gets __dso_handle from crtbegin.o and, on glibc, atexit,
// at_quick_exit and pthread_atfork from libc_nonshared.a: each registers with
// the IMAGE's handle, so the image's exit or unload (__cxa_finalize) runs
// exactly its own handlers. madc writes executables and shared objects itself
// and links JIT sessions in-process, with neither file, so these members take
// their place. Each is a few lines of C that madc_cir.cpp compiles once per
// process and a link pulls when its imports name a member's symbol, as ld
// pulls an archive member (cir_ledger_pull). Every definition is linkonce: a
// madc .o carrying one still links beside the system's crtbegin.o (whose
// strong definition wins), and two such objects merge.
//
// Windows exports __cxa_atexit from no DLL (mingw keeps it in libmingw32.a),
// so its members keep the image's list: the image's init hands the list's
// runner to the UCRT's exit; a JIT session, which runs no image init,
// finalizes the list itself before its code goes. Its at_quick_exit, also
// static in mingw, forwards to the UCRT's _crt_at_quick_exit.
//
// Thread safety: the table is immutable. glibc's and libSystem's
// __cxa_atexit lock; the Windows list mutates under an SRW lock.

#include "datadef.h"	// MADC_TARGET_WINDOWS_P / MADC_TARGET_APPLE_P
#include "madc_cir.h"

// The image handle. Its address identifies the image; its value is unused.
static const char crt_dso_handle[] =
	"__attribute__((linkonce)) void *__dso_handle = 0;\n";

static const char crt_atexit[] =
	"extern void *__dso_handle;\n"
	"int __cxa_atexit(void (*)(void *), void *, void *);\n"
	"__attribute__((linkonce)) int atexit(void (*f)(void))\n"
	"{\n"
	"	return __cxa_atexit((void (*)(void *))f, 0, &__dso_handle);\n"
	"}\n";

#if MADC_TARGET_WINDOWS_P
// __cxa_finalize(dso) runs the handlers registered with dso (all of them for
// a null dso), newest first, including any a handler registers.
static const char crt_exit_list[] =
	"struct __madc_exit_entry { void (*fn)(void *); void *arg; void *dso;\n"
	"	struct __madc_exit_entry *next; };\n"
	"void *malloc(unsigned long long);\n"
	"void free(void *);\n"
	"void AcquireSRWLockExclusive(void *);\n"
	"void ReleaseSRWLockExclusive(void *);\n"
	"int _crt_atexit(void (*)(void));\n"
	"__attribute__((linkonce)) struct __madc_exit_entry *__madc_exit_list = 0;\n"
	"__attribute__((linkonce)) void *__madc_exit_lock = 0;\n"
	"__attribute__((linkonce)) int __madc_exit_hooked = 0;\n"
	"__attribute__((linkonce)) int __cxa_atexit(void (*fn)(void *), void *arg,\n"
	"	void *dso)\n"
	"{\n"
	"	struct __madc_exit_entry *e = malloc(sizeof *e);\n"
	"	if (!e)\n"
	"		return -1;\n"
	"	e->fn = fn;\n"
	"	e->arg = arg;\n"
	"	e->dso = dso;\n"
	"	AcquireSRWLockExclusive(&__madc_exit_lock);\n"
	"	e->next = __madc_exit_list;\n"
	"	__madc_exit_list = e;\n"
	"	ReleaseSRWLockExclusive(&__madc_exit_lock);\n"
	"	return 0;\n"
	"}\n"
	"__attribute__((linkonce)) void __cxa_finalize(void *dso)\n"
	"{\n"
	"	for (;;) {\n"
	"		struct __madc_exit_entry **p, *e = 0;\n"
	"		AcquireSRWLockExclusive(&__madc_exit_lock);\n"
	"		for (p = &__madc_exit_list; *p; p = &(*p)->next)\n"
	"			if (!dso || (*p)->dso == dso) {\n"
	"				e = *p;\n"
	"				*p = e->next;\n"
	"				break;\n"
	"			}\n"
	"		ReleaseSRWLockExclusive(&__madc_exit_lock);\n"
	"		if (!e)\n"
	"			return;\n"
	"		e->fn(e->arg);\n"
	"		free(e);\n"
	"	}\n"
	"}\n"
	"__attribute__((linkonce)) void __madc_exit_run(void)\n"
	"{\n"
	"	__cxa_finalize(0);\n"
	"}\n"
	"__attribute__((linkonce)) void __madc_crt_init(int argc, char **argv,\n"
	"	char **envp)\n"
	"{\n"
	"	if (__madc_exit_hooked)\n"
	"		return;\n"
	"	__madc_exit_hooked = 1;\n"
	"	_crt_atexit(__madc_exit_run);\n"
	"}\n";

// The UCRT exports _crt_at_quick_exit; mingw's static CRT spells it
// at_quick_exit.
static const char crt_at_quick_exit[] =
	"int _crt_at_quick_exit(void (*)(void));\n"
	"__attribute__((linkonce)) int at_quick_exit(void (*f)(void))\n"
	"{\n"
	"	return _crt_at_quick_exit(f);\n"
	"}\n";
#elif !MADC_TARGET_APPLE_P
static const char crt_at_quick_exit[] =
	"extern void *__dso_handle;\n"
	"int __cxa_at_quick_exit(void (*)(void *), void *);\n"
	"__attribute__((linkonce)) int at_quick_exit(void (*f)(void))\n"
	"{\n"
	"	return __cxa_at_quick_exit((void (*)(void *))f, &__dso_handle);\n"
	"}\n";

static const char crt_pthread_atfork[] =
	"extern void *__dso_handle;\n"
	"int __register_atfork(void (*)(void), void (*)(void), void (*)(void),\n"
	"	void *);\n"
	"__attribute__((linkonce)) int pthread_atfork(void (*prepare)(void),\n"
	"	void (*parent)(void), void (*child)(void))\n"
	"{\n"
	"	return __register_atfork(prepare, parent, child, &__dso_handle);\n"
	"}\n";
#endif

const madc_crt_member madc_crt_members[] = {
	{ "crt:dso_handle", "__dso_handle", NULL, crt_dso_handle },
	{ "crt:atexit", "atexit", NULL, crt_atexit },
#if MADC_TARGET_WINDOWS_P
	{ "crt:exit_list", "__cxa_atexit __cxa_finalize __madc_exit_run"
	  " __madc_crt_init __madc_exit_list __madc_exit_lock"
	  " __madc_exit_hooked", "__madc_crt_init", crt_exit_list },
	{ "crt:at_quick_exit", "at_quick_exit", NULL, crt_at_quick_exit },
#elif !MADC_TARGET_APPLE_P
	{ "crt:at_quick_exit", "at_quick_exit", NULL, crt_at_quick_exit },
	{ "crt:pthread_atfork", "pthread_atfork", NULL, crt_pthread_atfork },
#endif
	{ NULL, NULL, NULL, NULL }
};
