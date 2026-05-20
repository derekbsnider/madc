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
DEFAULT_UNSUPPORTED_TARGETS = {
	"label_values",
	"trampolines",
}


class Result:
	def __init__(self, path, status, reason="", output=""):
		self.path = path
		self.status = status
		self.reason = reason
		self.output = output


def repo_root():
	return Path(__file__).resolve().parent.parent


def default_tests(root):
	execute_dir = root / "gcc.c-torture" / "execute"
	return sorted(p for p in execute_dir.glob("*.c") if p.is_file())


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


def builtin_multifile_source(path):
	path = Path(path)
	if path.parent.name != "builtins":
		return None
	companion = path.with_name(path.stem + "-lib.c")
	main_driver = path.parent / "lib" / "main.c"
	if not companion.exists() or not main_driver.exists():
		return None
	tmp = tempfile.NamedTemporaryFile(
		mode="w",
		suffix=".c",
		prefix="madc-gcc-builtins-",
		delete=False,
	)
	with tmp:
		tmp.write('#include "' + companion.resolve().as_posix() + '"\n')
		tmp.write('#include "' + path.resolve().as_posix() + '"\n')
		tmp.write('#include "' + main_driver.resolve().as_posix() + '"\n')
	return Path(tmp.name)


def run_one(path, madc, timeout):
	temp_source = builtin_multifile_source(path)
	input_path = temp_source if temp_source else path
	try:
		completed = subprocess.run(
			[str(madc), str(input_path)],
			stdout=subprocess.PIPE,
			stderr=subprocess.STDOUT,
			text=True,
			timeout=timeout,
			check=False,
		)
	except subprocess.TimeoutExpired as exc:
		output = exc.stdout or ""
		if exc.stderr:
			output += exc.stderr
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

	status, reason = classify_output(completed.returncode, completed.stdout)
	return Result(path, status, reason, completed.stdout)


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
	parser.add_argument("--timeout", type=float, default=5.0, help="seconds per test")
	parser.add_argument("--limit", type=int, default=0, help="limit discovered tests")
	parser.add_argument("--verbose", action="store_true", help="print passing tests and diagnostics")
	parser.add_argument(
		"--include-unsupported",
		action="store_true",
		help="run tests with known unsupported dg target requirements",
	)
	return parser.parse_args(argv)


def main(argv):
	args = parse_args(argv)
	root = Path(args.root)
	madc = Path(args.madc)

	if not root.exists():
		print("gcc testsuite root not found: " + str(root), file=sys.stderr)
		return 2
	if not madc.exists():
		print("madc binary not found: " + str(madc), file=sys.stderr)
		return 2

	if args.tests:
		tests = [Path(t) for t in args.tests]
	else:
		tests = default_tests(root)
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

	for test in tests:
		if not test.exists():
			result = Result(test, "FAIL(harness)", "missing file")
		else:
			reason = skip_reason(test, unsupported_targets)
			if reason:
				result = Result(test, "SKIP", reason)
			else:
				result = run_one(test, madc, args.timeout)
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
