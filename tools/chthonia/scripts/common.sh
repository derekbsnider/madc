# common.sh — what Chthonia's scripts share; each sources it.
#
#   here    Chthonia's top directory
#   madc    MADC, the madc to use, as a command (default: madc on PATH;
#           "wine <madc folder>/bin/madc.exe" for the Windows one)
#   exe     the executable suffix of madc's platform ("" or ".exe")
#   chthonia_setup
#           sets inc (madcide's public headers) and bundle (Chthonia's
#           bundle), both absolute, or says why it cannot
#   madcide_cmd
#           the madcide beside madc, as a command (MADCIDE overrides)
#   refresh_sums DIST FILE...
#           DIST/SHA256SUMS's lines for the packages just made
#   plain_modes DIR
#           DIR's tree with the modes a package installs
#   capped SECONDS COMMAND...
#           COMMAND under a wall-clock and a CPU limit
#
# madcide's public headers are MADCIDE_INCLUDE, else the madc install's:
# <prefix>/share/madcide/include (Linux, macOS), or the include directory
# beside madc.exe (Windows, where madcide's data sits beside the programs).
# Chthonia's bundle is the directory of its plugin's code, the unit
# chthonia.json names.

here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
madc=${MADC:-madc}

# The madc program is the command's last word; one with a directory becomes
# absolute, since the scripts change directory.
madc_prog=${madc##* }
madc_pre=
[ "$madc_prog" != "$madc" ] && madc_pre="${madc% *} "
case "$madc_prog" in
/*|[A-Za-z]:*) ;;
*/*) madc_prog=$(cd "$(dirname "$madc_prog")" && pwd)/$(basename "$madc_prog")
     madc="$madc_pre$madc_prog" ;;
esac
exe=
case "$madc_prog" in
*.exe) exe=.exe ;;
esac

# The directory holding the madc program ("" = madc is not found).
madc_bindir() {
	local p=$madc_prog
	case "$p" in
	*/*) ;;
	*) p=$(command -v "$p") || return 0 ;;
	esac
	(cd "$(dirname "$p")" 2>/dev/null && pwd)
}

# madcide's installed data directory SUB beside madc: <prefix>/share/madcide/SUB,
# else <bindir>/SUB ("" = neither).
madcide_data() {
	local b
	b=$(madc_bindir)
	[ -n "$b" ] || return 0
	if [ -d "$b/../share/madcide/$1" ]; then
		(cd "$b/../share/madcide/$1" && pwd)
	elif [ -d "$b/$1" ]; then
		echo "$b/$1"
	fi
}

chthonia_setup() {
	inc=${MADCIDE_INCLUDE:-}
	[ -n "$inc" ] || inc=$(madcide_data include)
	if [ -z "$inc" ]; then
		echo "$(basename "$0"): no madcide headers beside madc ($madc) — set MADC or MADCIDE_INCLUDE" >&2
		return 1
	fi
	if ! inc=$(cd "$inc" 2>/dev/null && pwd) || [ ! -f "$inc/madcide/harness" ]; then
		echo "$(basename "$0"): no <madcide/harness> in ${MADCIDE_INCLUDE:-$inc} — set MADCIDE_INCLUDE" >&2
		return 1
	fi
	local code
	code=$(sed -n 's|^[[:space:]]*"\(.*\)/chthonia\.mad",\{0,1\}[[:space:]]*$|\1|p' "$here/chthonia.json")
	if ! bundle=$(cd "$here/$code" 2>/dev/null && pwd) || [ ! -f "$bundle/chthonia.plugin" ]; then
		echo "$(basename "$0"): chthonia.json's plugin unit ($code/chthonia.mad) is not beside chthonia.plugin" >&2
		return 1
	fi
}

madcide_cmd() {
	if [ -n "${MADCIDE:-}" ]; then
		echo "$MADCIDE"
		return
	fi
	local b
	b=$(madc_bindir)
	echo "$madc_pre${b:+$b/}madcide$exe"
}

# refresh_sums DIST FILE... — DIST/SHA256SUMS gets these files' lines and
# keeps every other package's (each platform packages on its own host).
refresh_sums() {
	(
		cd "$1" || exit 1
		shift
		touch SHA256SUMS
		for f in "$@"; do
			awk -v f="$f" '$2 != f' SHA256SUMS > SHA256SUMS.tmp
			mv SHA256SUMS.tmp SHA256SUMS
		done
		if command -v sha256sum > /dev/null; then
			sha256sum "$@" >> SHA256SUMS
		else
			shasum -a 256 "$@" >> SHA256SUMS
		fi
	)
}

# plain_modes DIR — directories 0755 (a setgid bit inherited from the
# checkout cleared first, since a numeric mode keeps it), executables 0755,
# every other file 0644: what a package installs, whatever the checkout's
# modes and umask were.
plain_modes() {
	find "$1" -type d -exec chmod g-s {} +
	find "$1" -type d -exec chmod 0755 {} +
	find "$1" -type f -perm -0100 -exec chmod 0755 {} +
	find "$1" -type f ! -perm -0100 -exec chmod 0644 {} +
}

# capped SECONDS COMMAND... — COMMAND under a wall-clock limit (coreutils'
# timeout; gtimeout where Homebrew's coreutils provides it, as on a Mac) and
# the same CPU limit where the shell can set one (Git Bash on Windows cannot),
# so a hang fails instead of running on.
tmo=$(command -v timeout || command -v gtimeout || true)
capped() {
	if [ -z "$tmo" ]; then
		echo "$(basename "$0"): no timeout command (coreutils: timeout, or gtimeout on a Mac)" >&2
		return 124
	fi
	local s=$1
	shift
	(ulimit -t "$s" 2> /dev/null || true; "$tmo" "$s" "$@")
}
