/* win_ucrt_compat.S — UCRT compat thunks for the win64 release lane (Track 6.4).
 *
 * Linked into every win64 artifact built with the UCRT specs swap
 * (scripts/win_ucrt_gate.sh documents the recipe).  msvcrt.dll exports
 * _setjmp/_setjmpex on x64; ucrtbase exports __intrinsic_setjmp(ex) instead.
 * Static libs compiled against msvcrt-default headers (Ubuntu's winpthreads,
 * libstdc++) reference the msvcrt names, so linking them over -lucrt leaves
 * _setjmp undefined.  mingw's own setjmp.h maps _setjmp -> __intrinsic_setjmpex
 * under _UCRT+_WIN64, so a tail-jmp thunk is semantics-preserving: both take
 * (jmp_buf, frame-ctx) with the ctx in rdx passed straight through, and the
 * SEH-unwind-vs-plain-restore decision rides that ctx at longjmp time.
 */
	.text
	.globl	_setjmp
_setjmp:
	jmp	__intrinsic_setjmpex
	.globl	_setjmpex
_setjmpex:
	jmp	__intrinsic_setjmpex
