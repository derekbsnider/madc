#!/usr/bin/env python3
"""release_diff.py -- the measuring and verdict half of scripts/release_diff.sh.

A is the previous release's archived madc-release and B is the candidate. Each
stage runs both on the same machine, interleaved (a sequential A-then-B run
penalises whichever goes second on a cold cache), and prints one verdict line

    release_diff: <stage> GREEN|RED -- <summary>

then writes its rows to OUT/<stage>.txt. Exit 1 when the stage is RED, 2 on a
usage or harness error.

    release_diff.py exec     A B OUT    execution benchmarks, outputs compared,
                                        every mover shift-tested
    release_diff.py compile  A B OUT    compile wall time (-c, -o, JIT)
    release_diff.py memory   A B OUT    peak RSS of the same compiles
    release_diff.py startup  A B OUT    --version, a JIT hello, a hello image
    release_diff.py cost     ADIR BDIR OUT   two parse_cost_gate.sh --map dirs
    release_diff.py tests    OUT        OUT/tests-{A,B}.jsonl + OUT/tests-{A,B}/
    release_diff.py packages ADIR BDIR OUT   two release asset directories

A RED row is excused for ONE candidate version by a line of
docs/parity/release-diff-allow.tsv: version, stage, key, reason (tab
separated); the reason is printed beside the row.

Every child runs under a wall-clock timeout AND an RLIMIT_CPU cap.
"""
import glob
import json
import math
import os
import re
import resource
import shlex
import statistics
import subprocess
import sys
import tarfile
import time
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALLOW = os.path.join(ROOT, 'docs/parity/release-diff-allow.tsv')

N = int(os.environ.get('RELEASE_DIFF_N', '5'))	# rounds per timing (best of N)
# an exec row slower than this is a mover; still slower after the placement
# average is a real regression (the env override exists to force the shift
# test in a harness check: RELEASE_DIFF_EXEC_TOL=-0.5 makes every row a mover)
EXEC_TOL = float(os.environ.get('RELEASE_DIFF_EXEC_TOL', '0.03'))
# RELEASE_DIFF_ONLY: comma-separated benchmark names, to run a subset
ONLY = set(x for x in os.environ.get('RELEASE_DIFF_ONLY', '').split(',') if x)
EXEC_GEO_TOL = 0.015	# the exec geometric mean
SHIFTS = 5		# placements in a shift test: 0..4 padding functions
COMPILE_GEO_TOL = 0.03	# the compile geometric mean
COMPILE_ROW_TOL = 0.10	# a compile row slower than this is listed
RSS_TOL = 0.05		# peak RSS growth that is RED ...
RSS_MIN_KB = 4096	# ... when it is also at least this many KB
STARTUP_TOL = 0.10	# startup growth that is RED ...
STARTUP_MIN_MS = 2.0	# ... when it is also at least this much per run
COST_TOL = 0.005	# callgrind Ir growth (parse_cost_gate.sh's own tolerance)
PKG_SIZE_TOL = 0.20	# a package whose size moves more than this is listed

PAD = 'int madc_release_diff_pad%d(int x) { return x * 3 + %d; }\n'


def capped(cpu):
	def limit():
		resource.setrlimit(resource.RLIMIT_CPU, (cpu, cpu))
	return limit


def run(cmd, timeout=600, cwd=None, out=subprocess.PIPE):
	"""(rc, stdout, stderr); rc 124 on a timeout, like timeout(1)."""
	try:
		p = subprocess.run(cmd, stdout=out, stderr=subprocess.PIPE,
				   stdin=subprocess.DEVNULL, timeout=timeout,
				   preexec_fn=capped(timeout), cwd=cwd)
		return p.returncode, p.stdout or b'', p.stderr
	except subprocess.TimeoutExpired as e:
		return 124, e.stdout or b'', e.stderr or b''


def timed(cmd, timeout=600, cwd=None):
	"""(seconds, rc) of one run, stdout discarded."""
	t = time.perf_counter()
	rc, _, _ = run(cmd, timeout, cwd, out=subprocess.DEVNULL)
	return time.perf_counter() - t, rc


def rusage_run(cmd, timeout=600):
	"""(rc, peak RSS in KB) of one run, from wait4."""
	p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
			     stdin=subprocess.DEVNULL, preexec_fn=capped(timeout))
	deadline = time.monotonic() + timeout
	while True:
		pid, status, ru = os.wait4(p.pid, os.WNOHANG)
		if pid:
			return os.waitstatus_to_exitcode(status), ru.ru_maxrss
		if time.monotonic() > deadline:
			p.kill()
			_, status, ru = os.wait4(p.pid, 0)
			return 124, ru.ru_maxrss
		time.sleep(0.01)


def interleaved(fa, fb, rounds):
	"""best-of-rounds seconds of fa() and fb(), alternating which goes first."""
	ta, tb = [], []
	fa()
	fb()			# warm the page cache
	for r in range(rounds):
		for f, acc in ((fa, ta), (fb, tb)) if r % 2 == 0 else ((fb, tb), (fa, ta)):
			acc.append(f())
	return min(ta), min(tb)


def version_of(madc):
	rc, out, _ = run([madc, '--version'], timeout=60)
	line = out.decode(errors='replace').splitlines()[:1]
	return line[0].split()[-1] if line else '?'


def allowed(version):
	"""{(stage, key): reason} for this candidate version."""
	got = {}
	if os.path.exists(ALLOW):
		for line in open(ALLOW, encoding='utf-8'):
			if not line.strip() or line.startswith('#'):
				continue
			f = line.rstrip('\n').split('\t')
			if len(f) >= 4 and f[0].lstrip('v') == version.lstrip('v'):
				got[(f[1], f[2])] = f[3]
	return got


class Stage:
	"""Rows, RED keys and the verdict of one stage."""

	def __init__(self, name, out, version):
		self.name, self.out = name, out
		self.rows, self.red, self.notes = [], [], []
		self.allow = allowed(version)

	def row(self, text):
		self.rows.append(text)

	def fail(self, key, text):
		why = self.allow.get((self.name, key))
		if why:
			self.rows.append('%s   [allowed: %s]' % (text, why))
		else:
			self.rows.append(text + '   RED')
			self.red.append(key)

	def finish(self, summary):
		verdict = 'RED' if self.red else 'GREEN'
		head = 'release_diff: %s %s -- %s' % (self.name, verdict, summary)
		if self.red:
			head += ' -- red: ' + ' '.join(self.red[:20]) + (' ...' if len(self.red) > 20 else '')
		with open(os.path.join(self.out, self.name + '.txt'), 'w', encoding='utf-8') as f:
			f.write(head + '\n')
			for line in self.notes + self.rows:
				f.write(line + '\n')
		print(head)
		for line in self.notes:
			print('  ' + line)
		return 1 if self.red else 0


def geomean(ratios):
	return math.exp(sum(math.log(r) for r in ratios) / len(ratios)) if ratios else 1.0


# -- the corpora ------------------------------------------------------------

def benchmarks():
	"""(name, source, std flags, argv): benchmark_lane.sh's three corpora."""
	rows = []
	mirb = os.path.join(ROOT, 'third_party/mir/c-benchmarks')
	for src in sorted(glob.glob(os.path.join(mirb, '*.c'))):
		name = os.path.basename(src)[:-2]
		argf = os.path.join(mirb, name + '.arg')
		if not os.path.exists(argf):
			continue	# not a benchmark (oggenc's driver, a helper)
		rc, out, _ = run(['sh', '-c', open(argf).read()], timeout=60, cwd=ROOT)
		rows.append((name, src, ['--std=gnu17'], shlex.split(out.decode())))
	rows.append(('fp-callchain', os.path.join(ROOT, 'docs/benchmarks/fp-callchain.c'),
		     ['--std=gnu17'], ['20000']))
	for src in sorted(glob.glob(os.path.join(ROOT, 'docs/benchmarks/cpp/*.cpp'))):
		rows.append((os.path.basename(src)[:-4], src, ['--std=c++11'], ['20000']))
	return [r for r in rows if not ONLY or r[0] in ONLY]


def compiles(work):
	"""(name, argv-after-madc): every compile the compile and memory stages time.
	The benchmarks as -c and as -o, the parse-cost C and C++ workloads as -c, the
	parse-cost dialect workloads as a JIT run."""
	rows = []
	for name, src, std, _ in benchmarks():
		rows.append((name + ' -c', std + ['-c', '-o', os.path.join(work, 'c.o'), src]))
		rows.append((name + ' -o', std + ['-o', os.path.join(work, 'x'), src]))
	pc = os.path.join(ROOT, 'scripts/parse_cost')
	for src in sorted(glob.glob(os.path.join(pc, '*.c')) + glob.glob(os.path.join(pc, '*.cpp'))):
		if not os.path.exists(src):
			continue	# a workload symlink whose source is not on this host
		rows.append((os.path.basename(src) + ' -c', ['-c', '-o', os.path.join(work, 'w.o'), src]))
	for src in sorted(glob.glob(os.path.join(pc, '*.mad'))):
		rows.append((os.path.basename(src) + ' jit', [src]))
	return rows


# -- exec -------------------------------------------------------------------

def build(madc, std, src, image, extra=()):
	rc, _, err = run([madc] + std + ['-O2'] + list(extra) + ['-o', image, src])
	return rc == 0


def exec_pair(a_img, b_img, argv):
	return interleaved(lambda: timed([a_img] + argv, 300)[0],
			   lambda: timed([b_img] + argv, 300)[0], N)


def shift_test(A, B, name, src, std, argv, work):
	"""Mean B/A over SHIFTS placements: the source with 0..SHIFTS-1 padding
	functions ahead of it moves every function the source defines. A row whose
	slowdown is placement averages out; a real one does not."""
	text = open(src, encoding='utf-8', errors='replace').read()
	sa = sb = 0.0
	per = []
	for p in range(SHIFTS):
		shifted = os.path.join(work, 'shift_%s.%s' % (name, src.rsplit('.', 1)[1]))
		with open(shifted, 'w', encoding='utf-8') as f:
			f.write(''.join(PAD % (i, i) for i in range(p)) + text)
		inc = ['-I' + os.path.dirname(src)]
		ai, bi = os.path.join(work, 'sa_' + name), os.path.join(work, 'sb_' + name)
		if not (build(A, std, shifted, ai, inc) and build(B, std, shifted, bi, inc)):
			return None, 'shift build failed at placement %d' % p
		ta, tb = exec_pair(ai, bi, argv)
		sa += ta
		sb += tb
		per.append('%.3f' % (tb / ta))
	return sb / sa, ' '.join(per)


def stage_exec(A, B, out):
	st = Stage('exec', out, version_of(B))
	work = os.path.join(out, 'exec-work')
	os.makedirs(work, exist_ok=True)
	ratios = []
	movers = 0
	st.row('%-18s %9s %9s %8s  %s' % ('benchmark', 'A ms', 'B ms', 'B/A', 'verdict'))
	for name, src, std, argv in benchmarks():
		ai, bi = os.path.join(work, 'a_' + name), os.path.join(work, 'b_' + name)
		if not build(A, std, src, ai):
			st.row('%-18s A does not build it (not a regression)' % name)
			continue
		if not build(B, std, src, bi):
			st.fail(name, '%-18s B does not build it; A does' % name)
			continue
		ra = run([ai] + argv, 300)
		rb = run([bi] + argv, 300)
		if (ra[0], ra[1]) != (rb[0], rb[1]):
			st.fail(name, '%-18s output or exit status differs (A rc %d, B rc %d)' % (name, ra[0], rb[0]))
			continue
		ta, tb = exec_pair(ai, bi, argv)
		r = tb / ta
		line = '%-18s %9.1f %9.1f %8.3f' % (name, ta * 1000, tb * 1000, r)
		if r <= 1 + EXEC_TOL:
			ratios.append(r)
			st.row(line)
			continue
		movers += 1
		mean, per = shift_test(A, B, name, src, std, argv, work)
		if mean is None:
			st.fail(name, line + '  mover; ' + per)
			continue
		ratios.append(mean)
		detail = '  mover; placement-averaged %.3f over %d placements (%s)' % (mean, SHIFTS, per)
		if mean > 1 + EXEC_TOL:
			st.fail(name, line + detail + ' -- REAL')
		else:
			st.row(line + detail + ' -- placement')
	g = geomean(ratios)
	if g > 1 + EXEC_GEO_TOL:
		st.fail('geomean', 'geometric mean %.3f over %d rows' % (g, len(ratios)))
	return st.finish('geomean B/A %.3f over %d benchmarks, %d mover(s) shift-tested' % (g, len(ratios), movers))


# -- compile, memory, startup -------------------------------------------------

def stage_compile(A, B, out):
	st = Stage('compile', out, version_of(B))
	work = os.path.join(out, 'compile-work')
	os.makedirs(work, exist_ok=True)
	flush_images(A, B)
	ratios = []
	st.row('%-26s %9s %9s %8s' % ('compile', 'A ms', 'B ms', 'B/A'))
	for name, args in compiles(work):
		if run([A] + args)[0] != 0:
			st.row('%-26s A fails it (not a regression)' % name)
			continue
		if run([B] + args)[0] != 0:
			st.fail(name, '%-26s B fails it; A does not' % name)
			continue
		ta, tb = interleaved(lambda: timed([A] + args)[0], lambda: timed([B] + args)[0], N)
		r = tb / ta
		ratios.append(r)
		st.row('%-26s %9.1f %9.1f %8.3f%s' % (name, ta * 1000, tb * 1000, r,
			'  slower than %d%%' % (COMPILE_ROW_TOL * 100) if r > 1 + COMPILE_ROW_TOL else ''))
	g = geomean(ratios)
	if g > 1 + COMPILE_GEO_TOL:
		st.fail('geomean', 'geometric mean %.3f over %d compiles' % (g, len(ratios)))
	return st.finish('geomean B/A %.3f over %d compiles' % (g, len(ratios)))


def flush_images(*madcs):
	"""fsync + POSIX_FADV_DONTNEED each madc and every library beside it: a
	just-written libmadc.so reads ~13 MB high in peak RSS until its pages are
	flushed and dropped."""
	for madc in madcs:
		top = os.path.dirname(os.path.dirname(os.path.realpath(madc)))
		for path in [os.path.realpath(madc)] + glob.glob(os.path.join(top, 'lib', '**', '*.so*'), recursive=True):
			try:
				fd = os.open(path, os.O_RDONLY)
				try:
					os.fsync(fd)
					os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
				finally:
					os.close(fd)
			except OSError:
				pass


def stage_memory(A, B, out):
	st = Stage('memory', out, version_of(B))
	work = os.path.join(out, 'memory-work')
	os.makedirs(work, exist_ok=True)
	flush_images(A, B)
	worst = 0.0
	n = 0
	st.row('%-26s %9s %9s %8s' % ('compile', 'A KB', 'B KB', 'B/A'))
	for name, args in compiles(work):
		if name.endswith(' -o'):
			continue	# the -c row measures the same front end
		ra, rb = [], []
		for r in range(3):
			for madc, acc in ((A, ra), (B, rb)) if r % 2 == 0 else ((B, rb), (A, ra)):
				rc, kb = rusage_run([madc] + args)
				if rc == 0:
					acc.append(kb)
		if len(ra) < 3:
			st.row('%-26s A fails it (not a regression)' % name)
			continue
		if len(rb) < 3:
			st.fail(name, '%-26s B fails it; A does not' % name)
			continue
		ka, kb = statistics.median(ra), statistics.median(rb)
		r = kb / ka
		n += 1
		worst = max(worst, r)
		line = '%-26s %9d %9d %8.3f' % (name, ka, kb, r)
		if r > 1 + RSS_TOL and kb - ka >= RSS_MIN_KB:
			st.fail(name, line)
		else:
			st.row(line)
	return st.finish('peak RSS over %d compiles, worst B/A %.3f' % (n, worst))


def stage_startup(A, B, out):
	st = Stage('startup', out, version_of(B))
	work = os.path.join(out, 'startup-work')
	os.makedirs(work, exist_ok=True)
	flush_images(A, B)
	hello = os.path.join(work, 'hello.c')
	with open(hello, 'w') as f:
		f.write('#include <stdio.h>\nint main(void) { puts("hi"); return 0; }\n')
	ai, bi = os.path.join(work, 'hello_a'), os.path.join(work, 'hello_b')
	if not (build(A, [], hello, ai) and build(B, [], hello, bi)):
		st.fail('hello', 'a hello image does not build')
		return st.finish('no hello image')
	cases = [('madc --version', lambda m, i: [m, '--version'], 20),
		 ('madc hello.c (JIT)', lambda m, i: [m, hello], 5),
		 ('hello image', lambda m, i: [i], 20)]
	st.row('%-22s %10s %10s %8s' % ('per run', 'A ms', 'B ms', 'B/A'))
	for label, cmd, reps in cases:
		def loop(m, i):
			t = time.perf_counter()
			for _ in range(reps):
				run(cmd(m, i), 60, out=subprocess.DEVNULL)
			return (time.perf_counter() - t) / reps
		ta, tb = interleaved(lambda: loop(A, ai), lambda: loop(B, bi), N)
		line = '%-22s %10.2f %10.2f %8.3f' % (label, ta * 1000, tb * 1000, tb / ta)
		if tb > ta * (1 + STARTUP_TOL) and (tb - ta) * 1000 >= STARTUP_MIN_MS:
			st.fail(label, line)
		else:
			st.row(line)
	return st.finish('%d startup paths' % len(cases))


# -- cost -------------------------------------------------------------------

def totals(mapdir):
	got = {}
	path = os.path.join(mapdir, 'summary.tsv')
	if os.path.exists(path):
		for line in open(path, encoding='utf-8'):
			f = line.rstrip('\n').split('\t')
			if len(f) == 3 and f[0] == 'total':
				got[f[1]] = int(f[2])
	return got


def stage_cost(adir, bdir, out):
	st = Stage('cost', out, os.environ.get('RELEASE_DIFF_VERSION', '?'))
	ta, tb = totals(adir), totals(bdir)
	if not ta or not tb:
		st.fail('map', 'a parse-cost map is missing (A %d rows, B %d rows)' % (len(ta), len(tb)))
		return st.finish('no maps')
	st.row('%-20s %15s %15s %8s' % ('workload', 'A Ir', 'B Ir', 'B/A'))
	for row in sorted(set(ta) | set(tb)):
		if row not in tb:
			st.fail(row, '%-20s B has no measurement' % row)
			continue
		if row not in ta:
			st.row('%-20s %15s %15d  (new workload)' % (row, '-', tb[row]))
			continue
		r = tb[row] / ta[row]
		line = '%-20s %15d %15d %8.4f' % (row, ta[row], tb[row], r)
		if r > 1 + COST_TOL:
			st.fail(row, line)
		else:
			st.row(line)
	return st.finish('%d workloads, callgrind Ir, tolerance %.1f%%' % (len(tb), COST_TOL * 100))


# -- tests ------------------------------------------------------------------

def verdicts(path):
	got = {}
	for line in open(path, encoding='utf-8', errors='replace'):
		line = line.strip()
		if not line.startswith('{'):
			continue
		try:
			o = json.loads(line)
		except ValueError:
			continue
		if 'test' in o and 'result' in o and 'lane' not in o:
			got[o['test']] = o['result']
	return got


def saved(d, base, ext):
	p = os.path.join(d, base + ext)
	if not os.path.exists(p):
		return None
	with open(p, 'rb') as f:
		return f.read()


def stage_tests(out):
	st = Stage('tests', out, os.environ.get('RELEASE_DIFF_VERSION', '?'))
	va = verdicts(os.path.join(out, 'tests-A.jsonl'))
	vb = verdicts(os.path.join(out, 'tests-B.jsonl'))
	da, db = os.path.join(out, 'tests-A'), os.path.join(out, 'tests-B')
	if not va or not vb:
		st.fail('suite', 'a suite run produced no verdicts (A %d, B %d)' % (len(va), len(vb)))
		return st.finish('no verdicts')
	fixed, outdiff, errdiff, skipped = [], [], [], []
	for t in sorted(set(va) | set(vb)):
		a, b = va.get(t), vb.get(t)
		if a == 'pass' and b in ('fail', 'timeout'):
			st.fail(t, 'REGRESSED  %s: A passes, B %s' % (t, b))
			continue
		if a in ('fail', 'timeout') and b == 'pass':
			fixed.append(t)
		if a == 'pass' and b == 'skip':
			skipped.append(t)
		if a != 'pass' or b != 'pass':
			continue
		ea, eb = saved(da, t, '.err'), saved(db, t, '.err')
		if ea is not None and eb is not None:
			if not ea.strip() and eb.strip():
				first = eb.decode(errors='replace').strip().splitlines()[0][:160]
				st.fail(t, 'NEW STDERR %s: %s' % (t, first))
			elif ea != eb:
				errdiff.append(t)
		if saved(da, t, '.out') != saved(db, t, '.out'):
			outdiff.append(t)
	st.notes.append('%d tests in A, %d in B; fixed in B: %d; stdout differs: %d; '
			'stderr differs (both non-empty): %d; passed in A, skipped in B: %d'
			% (len(va), len(vb), len(fixed), len(outdiff), len(errdiff), len(skipped)))
	for label, names in (('stdout differs', outdiff), ('stderr differs', errdiff),
			     ('skipped in B', skipped), ('fixed in B', fixed)):
		for t in names:
			st.row('%-14s %s' % (label, t))
	return st.finish('%d regressed or newly noisy of %d both ran'
			 % (len(st.red), len(set(va) & set(vb))))


# -- packages ---------------------------------------------------------------

VERSION_RE = re.compile(r'\d+\.\d+\.\d+')


def norm(name):
	return VERSION_RE.sub('X', name.replace('~', '.'))


def listing(path):
	"""Normalized member paths of one package, or None when not a package."""
	if path.endswith(('.tar.gz', '.tgz')):
		with tarfile.open(path) as t:
			names = t.getnames()
	elif path.endswith('.zip'):
		with zipfile.ZipFile(path) as z:
			names = z.namelist()
	elif path.endswith('.deb'):
		rc, out, _ = run(['dpkg-deb', '-c', path], 120)
		names = [line.split()[-1] for line in out.decode(errors='replace').splitlines() if line.strip()]
	elif path.endswith('.rpm'):
		rc, out, _ = run(['rpm', '-qlp', path], 120)
		names = out.decode(errors='replace').splitlines()
	else:
		return None
	return set(norm(n.rstrip('/')) for n in names if n.strip('./'))


def stage_packages(adir, bdir, out):
	st = Stage('packages', out, os.environ.get('RELEASE_DIFF_VERSION', '?'))
	pa = {norm(os.path.basename(p)): p for p in glob.glob(os.path.join(adir, '*'))}
	pb = {norm(os.path.basename(p)): p for p in glob.glob(os.path.join(bdir, '*'))}
	n = 0
	for key in sorted(set(pa) | set(pb)):
		if key not in pb:
			st.fail(key, 'MISSING PACKAGE %s (A has %s)' % (key, os.path.basename(pa[key])))
			continue
		if key not in pa:
			st.row('new package %s' % os.path.basename(pb[key]))
			continue
		la, lb = listing(pa[key]), listing(pb[key])
		if la is None or lb is None:
			continue	# SHA256SUMS and the like
		n += 1
		sa, sb = os.path.getsize(pa[key]), os.path.getsize(pb[key])
		st.row('%-48s %6d -> %6d files, %9d -> %9d bytes (%+.1f%%)%s'
		       % (key, len(la), len(lb), sa, sb, (sb - sa) / sa * 100,
			  '  size moved' if abs(sb - sa) > sa * PKG_SIZE_TOL else ''))
		for m in sorted(la - lb):
			st.fail(key + ':' + m, '  missing in B: %s' % m)
		for m in sorted(lb - la):
			st.row('  new in B: %s' % m)
	return st.finish('%d packages compared, member lists and sizes' % n)


def main(argv):
	if len(argv) < 2:
		print(__doc__.strip(), file=sys.stderr)
		return 2
	stage, rest = argv[1], argv[2:]
	try:
		if stage in ('exec', 'compile', 'memory', 'startup') and len(rest) == 3:
			A, B, out = (os.path.abspath(x) for x in rest)
			return {'exec': stage_exec, 'compile': stage_compile, 'memory': stage_memory,
				'startup': stage_startup}[stage](A, B, out)
		if stage in ('cost', 'packages') and len(rest) == 3:
			return (stage_cost if stage == 'cost' else stage_packages)(*rest)
		if stage == 'tests' and len(rest) == 1:
			return stage_tests(rest[0])
	except Exception as e:	# a harness fault is never a verdict
		print('release_diff: %s HARNESS ERROR -- %s: %s' % (stage, type(e).__name__, e))
		return 2
	print(__doc__.strip(), file=sys.stderr)
	return 2


if __name__ == '__main__':
	sys.exit(main(sys.argv))
