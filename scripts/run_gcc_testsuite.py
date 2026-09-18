#!/usr/bin/env python3
"""Run GCC testsuite C execution tests through madc.

This is intentionally smaller than DejaGnu.  It uses GCC's execute tests as
plain C programs: success means madc compiles the file, runs it, and the
program exits normally.  GCC tests normally call abort() when the compiled
program computes the wrong answer, so a non-zero or signalled exit is a real
runtime failure.
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


COMPILE_ERROR_PATTERNS = (
	"error:",
	"Code generation failed",
	"No code generated",
	"Failed to ",
	"Exception:",
	"std::exception",
)

RUNTIME_ERROR_PATTERNS = (
	"caught SIG",
	"JIT frame on stack",
)

SKIP_DIRECTIVE_RE = re.compile(r"dg-require-effective-target\s+([A-Za-z0-9_-]+)")
DG_OPTIONS_RE = re.compile(r'dg-options\s+"([^"]*)"')
ADDITIONAL_FLAGS_RE = re.compile(r'set\s+additional_flags\s+"?([^"\n]+)"?')
DEFAULT_UNSUPPORTED_TARGETS = {
	"label_values",
	"trampolines",
}

# --- g++ suite (ROADMAP 2.12) -------------------------------------------------
# The C++11 lane measures PARSE/SEMA, so its in-scope set is the `dg-do compile`
# tests that must compile CLEAN. A test carrying dg-error / dg-bogus /
# dg-warning / dg-message asserts a DIAGNOSTIC — madc must produce that error,
# not merely fail — which is a later phase.
# See docs/plans/2026-09-04-cxx-conformance-lane.md.
# The DEFAULT scope is the C++11 lane (ROADMAP 2.12). --gxx-dirs re-points it
# at another standard's directories without touching this constant:
#   C++14 g++.dg/cpp1y   C++17 g++.dg/cpp1z   C++20 g++.dg/cpp2a
# Pair it with the matching --std=; the dirs and the standard are independent
# knobs because g++.dg/template is shared by every standard.
GXX_DIRS = ("g++.dg/cpp0x", "g++.dg/template")
DG_DO_COMPILE_RE = re.compile(r"dg-do\s+compile")
DG_DIAGNOSTIC_RE = re.compile(r"dg-(error|bogus|warning|message)")
DG_ADDITIONAL_SOURCES_RE = re.compile(r"dg-additional-sources")


class Result:
	def __init__(self, path, status, reason="", output=""):
		self.path = path
		self.status = status
		self.reason = reason
		self.output = output


def repo_root():
	return Path(__file__).resolve().parent.parent


def default_tests(root, suite="c-torture", gxx_dirs=None):
	if suite == "gxx":
		tests = []
		for rel in (gxx_dirs or GXX_DIRS):
			tests.extend(p for p in (root / rel).rglob("*.C") if p.is_file())
		return sorted(tests)
	execute_dir = root / "gcc.c-torture" / "execute"
	return sorted(p for p in execute_dir.glob("*.c") if p.is_file())


def gxx_scope_reason(path):
	"""Why this g++ test sits outside the compile-clean lane ('' = in scope)."""
	text = read_text(path)
	if not DG_DO_COMPILE_RE.search(text):
		return "not a dg-do compile test"
	if DG_DIAGNOSTIC_RE.search(text):
		return "diagnostic test (dg-error/bogus/warning/message)"
	if DG_ADDITIONAL_SOURCES_RE.search(text):
		return "dg-additional-sources companion TU"
	return ""


def as_text(data):
	"""subprocess.TimeoutExpired carries RAW BYTES even under text=True —
	CPython does not decode on the timeout path — so normalise before use."""
	if data is None:
		return ""
	if isinstance(data, bytes):
		return data.decode("utf-8", "replace")
	return data


def read_text(path):
	try:
		return path.read_text(errors="replace")
	except OSError:
		return ""


def skip_reason(path, unsupported_targets):
	text = read_text(path)
	for match in SKIP_DIRECTIVE_RE.finditer(text):
		target = match.group(1)
		if target in unsupported_targets:
			return "requires " + target
	return ""


def load_skip_manifest(manifest_path):
	"""Map test basename -> skip reason from the formal skip manifest."""
	skips = {}
	if not manifest_path or not Path(manifest_path).exists():
		return skips
	for line in read_text(Path(manifest_path)).splitlines():
		line = line.strip()
		if not line or line.startswith("#"):
			continue
		parts = line.split(None, 1)
		skips[parts[0]] = parts[1] if len(parts) > 1 else "manifest skip"
	return skips


def madc_args_from_directives(path):
	text = read_text(path)
	args = []
	sidecar = Path(path).with_suffix(".x")
	if sidecar.exists():
		for match in ADDITIONAL_FLAGS_RE.finditer(read_text(sidecar)):
			for opt in shlex.split(match.group(1)):
				if opt.startswith("-fno-builtin-"):
					args.append(opt)
	for match in DG_OPTIONS_RE.finditer(text):
		for opt in shlex.split(match.group(1)):
			if opt == "-finstrument-functions":
				args.append("--finstrument-functions")
			elif opt.startswith("-fno-builtin-"):
				args.append(opt)
	return args


def classify_output(returncode, output):
	if any(pattern in output for pattern in RUNTIME_ERROR_PATTERNS):
		return "FAIL(runtime)", "madc runtime diagnostic"
	if any(pattern in output for pattern in COMPILE_ERROR_PATTERNS):
		return "FAIL(compile)", "madc compile diagnostic"
	if returncode != 0:
		if returncode < 0:
			return "FAIL(runtime)", "terminated by signal " + str(-returncode)
		return "FAIL(runtime)", "exit " + str(returncode)
	return "PASS", ""


def is_builtin_companion_source(path):
	path = Path(path)
	if path.parent.name == "lib" and path.parent.parent.name == "builtins":
		return True
	return path.parent.name == "builtins" and path.name.endswith("-lib.c")


def filter_duplicate_aggregate_defs(body, seen_defs):
	lines = body.splitlines(keepends=True)
	out = []
	i = 0
	while i < len(lines):
		line = lines[i]
		stripped = line.lstrip()
		if stripped.startswith("struct ") or stripped.startswith("union "):
			block = [line]
			j = i + 1
			brace_balance = line.count("{") - line.count("}")
			while j < len(lines):
				block.append(lines[j])
				brace_balance += lines[j].count("{") - lines[j].count("}")
				if brace_balance <= 0 and lines[j].strip().endswith(";"):
					break
				j += 1
			block_text = "".join(block)
			if "{" in block_text and block_text.strip().endswith(";"):
				key = "".join(block_text.split())
				if key in seen_defs:
					i = j + 1
					continue
				seen_defs.add(key)
			out.extend(block)
			i = j + 1
			continue
		out.append(line)
		i += 1
	return "".join(out)


def absolutize_local_includes(body, base_dir):
	out = []
	for line in body.splitlines(keepends=True):
		stripped = line.strip()
		if stripped.startswith('#include "') and '"' in stripped[10:]:
			rel = stripped[len('#include "') : stripped.rfind('"')]
			resolved = (Path(base_dir) / rel).resolve()
			line = '#include "' + resolved.as_posix() + '"\n'
		out.append(line)
	return "".join(out)


def builtin_multifile_source(path):
	path = Path(path)
	if path.parent.name != "builtins":
		return None
	if path.name.endswith("-lib.c"):
		return None
	companion = path.with_name(path.stem + "-lib.c")
	main_driver = path.parent / "lib" / "main.c"
	if not companion.exists() or not main_driver.exists():
		return None
	companion_text = absolutize_local_includes(read_text(companion), companion.parent)
	test_text = absolutize_local_includes(read_text(path), path.parent)
	main_wrapper = """
int __madc_builtin_fail;
void abort(void) { if (!__madc_builtin_fail) __madc_builtin_fail = 1; }
void link_error(void) { if (!__madc_builtin_fail) __madc_builtin_fail = 2; }
extern void main_test(void);
int inside_main;
int main(void)
{
  inside_main = 1;
  main_test();
  inside_main = 0;
  return __madc_builtin_fail;
}
"""
	seen_defs = set()
	tmp = tempfile.NamedTemporaryFile(
		mode="w",
		suffix=".c",
		prefix="madc-gcc-builtins-",
		delete=False,
	)
	with tmp:
		tmp.write(filter_duplicate_aggregate_defs(companion_text, seen_defs))
		tmp.write("\n")
		tmp.write(filter_duplicate_aggregate_defs(test_text, seen_defs))
		tmp.write("\n")
		tmp.write(main_wrapper)
	return Path(tmp.name)


def run_one(path, madc, timeout, std, suite="c-torture"):
	temp_source = None if suite == "gxx" else builtin_multifile_source(path)
	input_path = temp_source if temp_source else path
	madc_args = (["--std=" + std] if std else []) + madc_args_from_directives(path)
	# The g++ lane is a PARSE/SEMA lane: --emit=c11 is madc's parse+sema+lower
	# driver and a TU with no main() exits 0, so PASS == exit 0 (the selfhost
	# lane's contract). stdout is then the EMITTED C11 — discard it, or a string
	# literal containing "error:" classifies a clean unit as a failure.
	# madc treats every argument AFTER the source as the program's argv, so the
	# source stays LAST.
	if suite == "gxx":
		madc_args = madc_args + ["--emit=c11"]
		out_stream, err_stream = subprocess.DEVNULL, subprocess.PIPE
	else:
		out_stream, err_stream = subprocess.PIPE, subprocess.STDOUT
	try:
		completed = subprocess.run(
			[str(madc)] + madc_args + [str(input_path)],
			stdout=out_stream,
			stderr=err_stream,
			text=True,
			timeout=timeout,
			check=False,
		)
	except subprocess.TimeoutExpired as exc:
		output = as_text(exc.stdout) + as_text(exc.stderr)
		if temp_source:
			try:
				os.unlink(temp_source)
			except OSError:
				pass
		return Result(path, "TIMEOUT", str(timeout) + "s", output)
	except OSError as exc:
		if temp_source:
			try:
				os.unlink(temp_source)
			except OSError:
				pass
		return Result(path, "FAIL(harness)", str(exc), "")
	finally:
		if temp_source:
			try:
				os.unlink(temp_source)
			except OSError:
				pass

	output = completed.stderr if suite == "gxx" else completed.stdout
	status, reason = classify_output(completed.returncode, output or "")
	return Result(path, status, reason, output or "")


def display_path(path):
	try:
		return path.relative_to(repo_root()).as_posix()
	except ValueError:
		return path.as_posix()


def print_result(result, verbose):
	display = display_path(result.path)
	if result.status == "PASS":
		if verbose:
			print("PASS: " + display)
		return
	if result.reason:
		print(result.status + ": " + display + " (" + result.reason + ")")
	else:
		print(result.status + ": " + display)
	if verbose and result.output:
		print(result.output.rstrip())


def parse_args(argv):
	root = repo_root()
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("tests", nargs="*", help="specific GCC testsuite .c files")
	parser.add_argument("--root", default=str(root / "gcc_testsuite"), help="GCC testsuite root")
	parser.add_argument("--madc", default=str(root / "bin" / "madc"), help="madc binary")
	parser.add_argument(
		"--suite",
		choices=("c-torture", "gxx"),
		default="c-torture",
		help="which third-party suite to drive: gcc.c-torture/execute (C, the "
		"promote gate) or g++.dg compile-clean (C++11 parity, ROADMAP 2.12)",
	)
	parser.add_argument(
		"--timeout",
		type=float,
		default=None,
		help="seconds per test (default 5 for c-torture, 30 for gxx — a C++ TU "
		"parses real system headers)",
	)
	parser.add_argument(
		"--std",
		default=None,
		help="--std= passed to madc for every test (default c17 for c-torture: "
		"C-era code, K&R/implicit-int recovery only exists under --std=c78..c17; "
		"default c++11 for gxx, because the lane proves C++11 rather than "
		"whatever madc defaults to); pass an empty string for the dialect default",
	)
	parser.add_argument(
		"--gxx-dirs",
		default=None,
		help="comma-separated testsuite dirs for --suite gxx, replacing the "
		"C++11 default (g++.dg/cpp0x,g++.dg/template). C++14 g++.dg/cpp1y, "
		"C++17 g++.dg/cpp1z, C++20 g++.dg/cpp2a — pair with the matching --std=",
	)
	parser.add_argument("--limit", type=int, default=0, help="limit discovered tests")
	parser.add_argument("--verbose", action="store_true", help="print passing tests and diagnostics")
	parser.add_argument(
		"--include-unsupported",
		action="store_true",
		help="run tests with known unsupported dg target requirements",
	)
	parser.add_argument(
		"--skip-manifest",
		default=None,
		help="formal skip manifest (class-(c) tests; see "
		"docs/parity/failset-classification.md); defaults to the manifest of the "
		"selected suite",
	)
	parser.add_argument(
		"--include-manifest-skips",
		action="store_true",
		help="run tests listed in the skip manifest anyway",
	)
	return parser.parse_args(argv)


def main(argv):
	args = parse_args(argv)
	root = Path(args.root)
	madc = Path(args.madc)
	suite = args.suite

	# Per-suite defaults, applied only when the flag was not given.
	std = args.std if args.std is not None else ("c++11" if suite == "gxx" else "c17")
	timeout = args.timeout if args.timeout is not None else (30.0 if suite == "gxx" else 5.0)
	skip_manifest = args.skip_manifest
	if skip_manifest is None:
		name = "gxx-skip-manifest.txt" if suite == "gxx" else "torture-skip-manifest.txt"
		skip_manifest = str(repo_root() / "docs" / "parity" / name)

	if not root.exists():
		print("gcc testsuite root not found: " + str(root), file=sys.stderr)
		return 2
	if not madc.exists():
		print("madc binary not found: " + str(madc), file=sys.stderr)
		return 2

	if args.tests:
		tests = [Path(t) for t in args.tests]
	else:
		gxx_dirs = ([d.strip() for d in args.gxx_dirs.split(",") if d.strip()]
			    if args.gxx_dirs else None)
		tests = default_tests(root, suite, gxx_dirs)
		if args.limit > 0:
			tests = tests[: args.limit]

	counts = {
		"PASS": 0,
		"FAIL(compile)": 0,
		"FAIL(runtime)": 0,
		"FAIL(harness)": 0,
		"TIMEOUT": 0,
		"SKIP": 0,
	}

	unsupported_targets = set() if args.include_unsupported else DEFAULT_UNSUPPORTED_TARGETS
	manifest_skips = {} if args.include_manifest_skips else load_skip_manifest(skip_manifest)

	for test in tests:
		if not test.exists():
			result = Result(test, "FAIL(harness)", "missing file")
		elif suite != "gxx" and is_builtin_companion_source(test):
			result = Result(test, "SKIP", "builtin companion source")
		elif test.name in manifest_skips:
			result = Result(test, "SKIP", "manifest: " + manifest_skips[test.name])
		else:
			reason = skip_reason(test, unsupported_targets)
			if not reason and suite == "gxx":
				reason = gxx_scope_reason(test)
			if reason:
				result = Result(test, "SKIP", reason)
			else:
				try:
					result = run_one(test, madc, timeout, std, suite)
				except Exception as exc:  # never let one test kill the lane
					result = Result(test, "FAIL(harness)",
						type(exc).__name__ + ": " + str(exc))
		counts[result.status] = counts.get(result.status, 0) + 1
		print_result(result, args.verbose)

	print(
		"{pass_count} passed, {compile_fail} compile-failed, "
		"{runtime_fail} runtime-failed, {timeouts} timed out, "
		"{skips} skipped".format(
			pass_count=counts["PASS"],
			compile_fail=counts["FAIL(compile)"],
			runtime_fail=counts["FAIL(runtime)"],
			timeouts=counts["TIMEOUT"],
			skips=counts["SKIP"],
		)
	)

	if counts["FAIL(compile)"] or counts["FAIL(runtime)"] or counts["FAIL(harness)"] or counts["TIMEOUT"]:
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))
