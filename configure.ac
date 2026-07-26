AC_PREREQ([2.69])
AC_INIT([madc], m4_esyscmd_s([cat VERSION]), [https://github.com/derekbsnider/madc/issues])
AC_CONFIG_SRCDIR([src/madc.cpp])
AC_CONFIG_HEADERS([include/config.h])

AC_PROG_CXX
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MAKE_SET

OPTIONAL_CPPFLAGS=""
OPTIONAL_LIBS=""
MADC_STORAGE_DB_OFILES=""
FEATURE_DEFINES=""
MADC_QDBM_CPPFLAGS=""
MADC_QDBM_LIBS=""
MADC_XQDBM_CPPFLAGS=""
MADC_XQDBM_LIBS=""

HAVE_MADCDAT=1
HAVE_BDB=0
HAVE_GDBM=0
HAVE_QDBM=0
HAVE_XQDBM=0
HAVE_SQLITE3=0

AC_SUBST([OPTIONAL_CPPFLAGS])
AC_SUBST([OPTIONAL_LIBS])
AC_SUBST([MADC_STORAGE_DB_OFILES])
AC_SUBST([FEATURE_DEFINES])
AC_SUBST([HAVE_MADCDAT])
AC_SUBST([HAVE_BDB])
AC_SUBST([HAVE_GDBM])
AC_SUBST([HAVE_QDBM])
AC_SUBST([HAVE_XQDBM])
AC_SUBST([HAVE_SQLITE3])

# zstd — the forest/pch container codec (zstd-preferred by design; the packed
# release binary depends on it for its size). REQUIRED by default: a missing
# dev package must fail configure, never silently degrade the pack to the
# zlib fallback (that silent degradation is how the packed release binary
# regressed 9MB -> 100MB). Opt out explicitly with --without-zstd.
AC_ARG_WITH([zstd],
    [AS_HELP_STRING([--without-zstd],
        [build without zstd; the packed forest falls back to zlib (larger binaries, slower binds — not recommended)])],
    [with_zstd="$withval"],
    [with_zstd=yes])
HAVE_ZSTD=0
if test "$with_zstd" != no; then
    AC_CHECK_HEADER([zstd.h], [madc_zstd_h=yes], [madc_zstd_h=no])
    AC_CHECK_LIB([zstd], [ZSTD_compress], [madc_zstd_lib=yes], [madc_zstd_lib=no])
    if test "$madc_zstd_h" = yes && test "$madc_zstd_lib" = yes; then
        HAVE_ZSTD=1
        OPTIONAL_CPPFLAGS="$OPTIONAL_CPPFLAGS -DHAVE_ZSTD"
        OPTIONAL_LIBS="$OPTIONAL_LIBS -lzstd"
    else
        AC_MSG_ERROR([zstd development files not found (install libzstd-dev), or pass --without-zstd to accept the zlib fallback (larger packed binaries, slower binds)])
    fi
fi
AC_SUBST([HAVE_ZSTD])

# Forest carrier shape (forest-carriers plan S3): what the PRODUCT build
# (make release + the hosted darwin MODEs) produces and expects — embedded
# packs the container into the binary's own image (the no-installation
# default), sidecar ships <binary>.forest beside it (the packaged-install
# shape), none skips the forest (live-parse only, the dev shape). This sets
# what the build PRODUCES and what the binary EXPECTS (the baked loud-notice
# default when the forest is missing at runtime) — never a restriction on
# runtime discovery: the probe chain accepts any carrier it finds.
AC_ARG_WITH([forest],
    [AS_HELP_STRING([--with-forest=embedded|sidecar|none],
        [forest carrier the product build ships (default: embedded)])],
    [with_forest="$withval"],
    [with_forest=embedded])
case "$with_forest" in
    embedded|sidecar|none) ;;
    *) AC_MSG_ERROR([bad value '$with_forest' for --with-forest (use embedded, sidecar, or none)]) ;;
esac
WITH_FOREST="$with_forest"
AC_SUBST([WITH_FOREST])

# Link shape (forest-carriers plan S4, configure axis 1): monolithic (default)
# links libmadc statically into the CLI — the single-binary, no-installation
# shape. --enable-shared links the CLI against the shared libmadc instead (the
# "thin CLI" of a packaged install): one libmadc image serves the CLI and every
# embedding host, and in the embedded forest carrier the container rides that
# LIBRARY image, so hosts get the groves for free. libmadc.so is built either
# way; this axis decides what the CLI links against.
AC_ARG_ENABLE([shared],
    [AS_HELP_STRING([--enable-shared],
        [link the madc CLI against the shared libmadc (thin CLI) instead of linking libmadc statically (default: no)])],
    [enable_shared_cli="$enableval"],
    [enable_shared_cli=no])
case "$enable_shared_cli" in
    yes) ENABLE_SHARED_CLI=1 ;;
    no)  ENABLE_SHARED_CLI=0 ;;
    *) AC_MSG_ERROR([bad value '$enable_shared_cli' for --enable-shared (use yes or no)]) ;;
esac
AC_SUBST([ENABLE_SHARED_CLI])

AC_ARG_ENABLE([madcdat],
    [AS_HELP_STRING([--enable-madcdat], [build madcdat storage/federation support @<:@default=yes@:>@])],
    [enable_madcdat="$enableval"],
    [enable_madcdat=yes])

AC_ARG_WITH([bdb],
    [AS_HELP_STRING([--with-bdb[=PREFIX]], [build Berkeley DB backend support])],
    [with_bdb="$withval"],
    [with_bdb=auto])

AC_ARG_WITH([gdbm],
    [AS_HELP_STRING([--with-gdbm[=PREFIX]], [build GDBM backend support])],
    [with_gdbm="$withval"],
    [with_gdbm=auto])

AC_ARG_WITH([qdbm],
    [AS_HELP_STRING([--with-qdbm[=PREFIX]], [build QDBM backend support])],
    [with_qdbm="$withval"],
    [with_qdbm=auto])

AC_ARG_WITH([xqdbm],
    [AS_HELP_STRING([--with-xqdbm[=PREFIX]], [build XQDBM C++ wrapper support])],
    [with_xqdbm="$withval"],
    [with_xqdbm=auto])

AC_ARG_WITH([sqlite3],
    [AS_HELP_STRING([--with-sqlite3[=PREFIX]], [build SQLite3 backend support])],
    [with_sqlite3="$withval"],
    [with_sqlite3=auto])

AC_DEFUN([MADC_APPEND_FLAGS], [
    if test -n "$2"; then
	if test -n "$1"; then
	    $1="[$]$1 $2"
	else
	    $1="$2"
	fi
    fi
])

AC_DEFUN([MADC_MARK_ENABLED], [
    FEATURE_DEFINES="$FEATURE_DEFINES -D$1=1"
    AC_DEFINE([$1], [1], [$2])
])

AC_DEFUN([MADC_PREPARE_PREFIX_FLAGS], [
    madc_pkg_cppflags=""
    madc_pkg_libs=""
    case "$1" in
      yes|auto) ;;
      no) ;;
      *)
	madc_pkg_cppflags="-I$1/include"
	madc_pkg_libs="-L$1/lib"
	;;
    esac
])

case "$enable_madcdat" in
  yes)
	MADC_MARK_ENABLED([HAVE_MADCDAT], [Define when madcdat storage/federation support is enabled.])
	;;
  no)
	HAVE_MADCDAT=0
	;;
  *)
	AC_MSG_ERROR([bad value '$enable_madcdat' for --enable-madcdat])
	;;
esac

AC_DEFUN([MADC_CHECK_BDB], [
    if test "$with_bdb" != no; then
	AC_MSG_CHECKING([for Berkeley DB support])
	MADC_PREPARE_PREFIX_FLAGS([$with_bdb])
	madc_save_CPPFLAGS="$CPPFLAGS"
	madc_save_LIBS="$LIBS"
	CPPFLAGS="$CPPFLAGS $madc_pkg_cppflags"
	LIBS="$madc_pkg_libs -ldb $LIBS"
	AC_LINK_IFELSE(
	    [AC_LANG_PROGRAM([[#include <db.h>]], [[DB *db = 0; db_create(&db, 0, 0);]])],
	    [madc_have_bdb=yes],
	    [madc_have_bdb=no])
	CPPFLAGS="$madc_save_CPPFLAGS"
	LIBS="$madc_save_LIBS"
	if test "$madc_have_bdb" = yes; then
	    HAVE_BDB=1
	    OPTIONAL_CPPFLAGS="$OPTIONAL_CPPFLAGS $madc_pkg_cppflags"
	    OPTIONAL_LIBS="$OPTIONAL_LIBS $madc_pkg_libs -ldb"
	    MADC_MARK_ENABLED([HAVE_BDB], [Define when Berkeley DB backend support is enabled.])
	    MADC_APPEND_FLAGS([MADC_STORAGE_DB_OFILES], [madcdat_storage_bdb.o])
	    AC_MSG_RESULT([yes])
	else
	    AC_MSG_RESULT([no])
	    case "$with_bdb" in
	      auto) ;;
	      *) AC_MSG_ERROR([Berkeley DB requested but db.h / libdb were not found]) ;;
	    esac
	fi
    fi
])

AC_DEFUN([MADC_CHECK_GDBM], [
    if test "$with_gdbm" != no; then
	AC_MSG_CHECKING([for GDBM support])
	MADC_PREPARE_PREFIX_FLAGS([$with_gdbm])
	madc_save_CPPFLAGS="$CPPFLAGS"
	madc_save_LIBS="$LIBS"
	CPPFLAGS="$CPPFLAGS $madc_pkg_cppflags"
	LIBS="$madc_pkg_libs -lgdbm $LIBS"
	AC_LINK_IFELSE(
	    [AC_LANG_PROGRAM([[#include <gdbm.h>]], [[GDBM_FILE db = gdbm_open((char *)"x", 0, GDBM_WRCREAT, 0600, 0);]])],
	    [madc_have_gdbm=yes],
	    [madc_have_gdbm=no])
	CPPFLAGS="$madc_save_CPPFLAGS"
	LIBS="$madc_save_LIBS"
	if test "$madc_have_gdbm" = yes; then
	    HAVE_GDBM=1
	    OPTIONAL_CPPFLAGS="$OPTIONAL_CPPFLAGS $madc_pkg_cppflags"
	    OPTIONAL_LIBS="$OPTIONAL_LIBS $madc_pkg_libs -lgdbm"
	    MADC_MARK_ENABLED([HAVE_GDBM], [Define when GDBM backend support is enabled.])
	    MADC_APPEND_FLAGS([MADC_STORAGE_DB_OFILES], [madcdat_storage_gdbm.o])
	    AC_MSG_RESULT([yes])
	else
	    AC_MSG_RESULT([no])
	    case "$with_gdbm" in
	      auto) ;;
	      *) AC_MSG_ERROR([GDBM requested but gdbm.h / libgdbm were not found]) ;;
	    esac
	fi
    fi
])

AC_DEFUN([MADC_CHECK_QDBM], [
    if test "$with_qdbm" != no; then
	AC_MSG_CHECKING([for QDBM support])
	MADC_PREPARE_PREFIX_FLAGS([$with_qdbm])
	madc_qdbm_cppflags="$madc_pkg_cppflags"
	case "$with_qdbm" in
	  yes|auto)
	    madc_qdbm_cppflags="$madc_qdbm_cppflags -I/usr/include/qdbm"
	    ;;
	  *)
	    madc_qdbm_cppflags="$madc_qdbm_cppflags -I$with_qdbm/include/qdbm"
	    ;;
	esac
	madc_save_CPPFLAGS="$CPPFLAGS"
	madc_save_LIBS="$LIBS"
	CPPFLAGS="$CPPFLAGS $madc_qdbm_cppflags"
	LIBS="$madc_pkg_libs -lqdbm $LIBS"
	AC_LINK_IFELSE(
	    [AC_LANG_PROGRAM([[#include <villa.h>]], [[VILLA *db = vlopen("x", VL_OWRITER | VL_OCREAT, VL_CMPLEX);]])],
	    [madc_have_qdbm=yes],
	    [madc_have_qdbm=no])
	CPPFLAGS="$madc_save_CPPFLAGS"
	LIBS="$madc_save_LIBS"
	if test "$madc_have_qdbm" = yes; then
	    HAVE_QDBM=1
	    MADC_QDBM_CPPFLAGS="$madc_qdbm_cppflags"
	    MADC_QDBM_LIBS="$madc_pkg_libs -lqdbm"
	    MADC_MARK_ENABLED([HAVE_QDBM], [Define when QDBM backend support is enabled.])
	    MADC_APPEND_FLAGS([MADC_STORAGE_DB_OFILES], [madcdat_storage_qdbm.o])
	    AC_MSG_RESULT([yes])
	else
	    AC_MSG_RESULT([no])
	    case "$with_qdbm" in
	      auto) ;;
	      *) AC_MSG_ERROR([QDBM Villa requested but villa.h / libqdbm were not found]) ;;
	    esac
	fi
    fi
])

AC_DEFUN([MADC_CHECK_XQDBM], [
    if test "$with_xqdbm" != no; then
	AC_MSG_CHECKING([for XQDBM support])
	MADC_PREPARE_PREFIX_FLAGS([$with_xqdbm])
	madc_xqdbm_cppflags="$madc_pkg_cppflags"
	case "$with_xqdbm" in
	  yes|auto)
	    madc_xqdbm_cppflags="$madc_xqdbm_cppflags -I/usr/include/qdbm"
	    ;;
	  *)
	    madc_xqdbm_cppflags="$madc_xqdbm_cppflags -I$with_xqdbm/include/qdbm"
	    ;;
	esac
	madc_save_CPPFLAGS="$CPPFLAGS"
	madc_save_LIBS="$LIBS"
	CPPFLAGS="$CPPFLAGS $madc_xqdbm_cppflags"
	LIBS="$madc_pkg_libs -lxqdbm -lqdbm -lpthread $LIBS"
	AC_LANG_PUSH([C++])
	AC_LINK_IFELSE(
	    [AC_LANG_PROGRAM([[#include <xqdbm.h>]], [[qdbm::Depot *db = 0; return db ? 1 : 0;]])],
	    [madc_have_xqdbm=yes],
	    [madc_have_xqdbm=no])
	AC_LANG_POP([C++])
	CPPFLAGS="$madc_save_CPPFLAGS"
	LIBS="$madc_save_LIBS"
	if test "$madc_have_xqdbm" = yes; then
	    HAVE_XQDBM=1
	    MADC_XQDBM_CPPFLAGS="$madc_xqdbm_cppflags"
	    MADC_XQDBM_LIBS="$madc_pkg_libs -lxqdbm -lqdbm -lpthread"
	    MADC_MARK_ENABLED([HAVE_XQDBM], [Define when XQDBM C++ wrapper support is enabled.])
	    AC_MSG_RESULT([yes])
	else
	    AC_MSG_RESULT([no])
	    case "$with_xqdbm" in
	      auto) ;;
	      *) AC_MSG_ERROR([XQDBM requested but xqdbm.h / libxqdbm were not found]) ;;
	    esac
	fi
    fi
])

AC_DEFUN([MADC_CHECK_SQLITE3], [
    if test "$with_sqlite3" != no; then
	AC_MSG_CHECKING([for SQLite3 support])
	MADC_PREPARE_PREFIX_FLAGS([$with_sqlite3])
	madc_save_CPPFLAGS="$CPPFLAGS"
	madc_save_LIBS="$LIBS"
	CPPFLAGS="$CPPFLAGS $madc_pkg_cppflags"
	LIBS="$madc_pkg_libs -lsqlite3 $LIBS"
	AC_LINK_IFELSE(
	    [AC_LANG_PROGRAM([[#include <sqlite3.h>]], [[sqlite3 *db = 0; sqlite3_open(":memory:", &db);]])],
	    [madc_have_sqlite3=yes],
	    [madc_have_sqlite3=no])
	CPPFLAGS="$madc_save_CPPFLAGS"
	LIBS="$madc_save_LIBS"
	if test "$madc_have_sqlite3" = yes; then
	    HAVE_SQLITE3=1
	    OPTIONAL_CPPFLAGS="$OPTIONAL_CPPFLAGS $madc_pkg_cppflags"
	    OPTIONAL_LIBS="$OPTIONAL_LIBS $madc_pkg_libs -lsqlite3"
	    MADC_MARK_ENABLED([HAVE_SQLITE3], [Define when SQLite3 backend support is enabled.])
	    MADC_APPEND_FLAGS([MADC_STORAGE_DB_OFILES], [madcdat_storage_sqlite.o])
	    AC_MSG_RESULT([yes])
	else
	    AC_MSG_RESULT([no])
	    case "$with_sqlite3" in
	      auto) ;;
	      *) AC_MSG_ERROR([SQLite3 requested but sqlite3.h / libsqlite3 were not found]) ;;
	    esac
	fi
    fi
])

if test "$HAVE_MADCDAT" = 1; then
    MADC_APPEND_FLAGS([MADC_STORAGE_DB_OFILES], [madcdat_storage.o])
    MADC_CHECK_BDB
    MADC_CHECK_GDBM
    MADC_CHECK_QDBM
    MADC_CHECK_XQDBM
    MADC_CHECK_SQLITE3
else
    case "$with_bdb" in
      auto|no) ;;
      *) AC_MSG_ERROR([--with-bdb requires --enable-madcdat]) ;;
    esac
    case "$with_gdbm" in
      auto|no) ;;
      *) AC_MSG_ERROR([--with-gdbm requires --enable-madcdat]) ;;
    esac
    case "$with_qdbm" in
      auto|no) ;;
      *) AC_MSG_ERROR([--with-qdbm requires --enable-madcdat]) ;;
    esac
    case "$with_xqdbm" in
      auto|no) ;;
      *) AC_MSG_ERROR([--with-xqdbm requires --enable-madcdat]) ;;
    esac
    case "$with_sqlite3" in
      auto|no) ;;
      *) AC_MSG_ERROR([--with-sqlite3 requires --enable-madcdat]) ;;
    esac
fi

if test -n "$MADC_QDBM_CPPFLAGS"; then
    OPTIONAL_CPPFLAGS="$OPTIONAL_CPPFLAGS $MADC_QDBM_CPPFLAGS"
fi
if test -n "$MADC_XQDBM_CPPFLAGS"; then
    case " $OPTIONAL_CPPFLAGS " in
      *" $MADC_XQDBM_CPPFLAGS "*) ;;
      *) OPTIONAL_CPPFLAGS="$OPTIONAL_CPPFLAGS $MADC_XQDBM_CPPFLAGS" ;;
    esac
fi

if test -n "$MADC_QDBM_LIBS"; then
    OPTIONAL_LIBS="$OPTIONAL_LIBS $MADC_QDBM_LIBS"
fi
if test -n "$MADC_XQDBM_LIBS"; then
    OPTIONAL_LIBS="$OPTIONAL_LIBS $MADC_XQDBM_LIBS"
fi

AC_MSG_NOTICE([madcdat=$HAVE_MADCDAT optional backends: bdb=$HAVE_BDB gdbm=$HAVE_GDBM qdbm=$HAVE_QDBM xqdbm=$HAVE_XQDBM sqlite3=$HAVE_SQLITE3])

AC_CONFIG_FILES([Makefile config.mk libmadc.pc])
AC_OUTPUT
