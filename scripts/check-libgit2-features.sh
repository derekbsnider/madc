#!/bin/bash
# GATE — libgit2 is vendored with every NETWORK transport OFF (owner decision
# 2026-09-13: the Nexus only READS local history; push/fetch stay git's).
# The committed feature headers under third_party/libgit2-madc/<platform>/ are
# the one place a transport could be switched on, and Makefile.madc is the one
# place a network dependency could be compiled in. Fail on either.
set -u
cd "$(dirname "$0")/.."

fail=0
net='GIT_(HTTPS|SSH|SSH_EXEC|SSH_LIBSSH2|SSH_LIBSSH2_MEMORY_CREDENTIALS|NTLM|GSSAPI|GSSFRAMEWORK|WINHTTP|OPENSSL|OPENSSL_DYNAMIC|SECURE_TRANSPORT|MBEDTLS|SCHANNEL)'

check_header() {
	# $1 = a git2_features.h; a network macro DEFINED (not "/* #undef */") fails
	local hits
	hits=$(grep -nE "^[[:space:]]*#[[:space:]]*define[[:space:]]+$net\b" "$1")
	if [ -n "$hits" ]; then
		echo "check-libgit2-features: network transport enabled in $1:"
		echo "$hits" | sed 's/^/  /'
		fail=1
	fi
}

found=0
for h in third_party/libgit2-madc/*/git2_features.h; do
	[ -f "$h" ] || continue
	found=1
	check_header "$h"
done
if [ "$found" -eq 0 ]; then
	echo "check-libgit2-features: no feature headers found under third_party/libgit2-madc/"
	exit 1
fi

deps=$(grep -nE 'deps/(ntlmclient|winhttp|zlib|chromium-zlib)' third_party/libgit2-madc/Makefile.madc | grep -vE '^[0-9]+:#')
if [ -n "$deps" ]; then
	echo "check-libgit2-features: Makefile.madc compiles a network/zlib dependency:"
	echo "$deps" | sed 's/^/  /'
	fail=1
fi

# Negative control: a header with HTTPS on must be caught, or the gate is dead.
ctrl=$(mktemp)
printf '#define GIT_THREADS 1\n#define GIT_HTTPS 1\n' > "$ctrl"
before=$fail
fail=0
check_header "$ctrl" >/dev/null
if [ "$fail" -ne 1 ]; then
	echo "check-libgit2-features: NEGATIVE CONTROL FAILED (GIT_HTTPS 1 not caught)"
	rm -f "$ctrl"
	exit 1
fi
fail=$before
rm -f "$ctrl"

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "check-libgit2-features: GREEN — every transport off in every platform header; no network dependency compiled."
exit 0
