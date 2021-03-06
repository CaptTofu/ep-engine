# ep-engine
# Copyright (C) 2010 Trond Norbye
# All rights reserved.
#
# Use and distribution licensed under the BSD license.  See
# the COPYING file in this directory for full text.
AC_PREREQ(2.59)
m4_include([m4/version.m4])
AC_INIT(epengine, VERSION_NUMBER, trond.norbye@gmail.com)
AC_CONFIG_SRCDIR([kvstore.hh])
AC_CONFIG_AUX_DIR(config)
AM_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_DISABLE_STATIC

PANDORA_CANONICAL_TARGET
PANDORA_SHARED_PTR

AC_DEFUN([AC_C_HTONLL],
[
    AC_CACHE_CHECK([for htonll],[av_cv_have_htonll],[
    AC_RUN_IFELSE([
       AC_LANG_PROGRAM([[
#include <sys/types.h>
#include <netinet/in.h>
#include <inttypes.h>
       ]],[[
          return htonll(0);
       ]])
    ], [
      ac_cv_have_htonll=yes
    ],[
      ac_cv_have_htonll=no
    ])])

    AS_IF([test "x$ac_cv_have_htonll" = "xyes"],[
      AC_DEFINE([HAVE_HTONLL], [1], [Have ntohll])])
])

AC_C_HTONLL

AC_CHECK_HEADERS([arpa/inet.h])

AC_SEARCH_LIBS(pthread_create, pthread)

AS_IF([test "x$SUNCC" = "xyes"],
      [
        CFLAGS="-D_XOPEN_SOURCE=600 $CFLAGS"
        CPPFLAGS="-xldscope=hidden -mt $CPPFLAGS"
        CXXFLAGS="-erroff=truncwarn1,signextwarn,notused,inllargeuse,wunreachable $CXXFLAGS"
      ])

AH_BOTTOM([
#if ((defined (__SUNPRO_C) || defined(__SUNPRO_CC)) || defined __GNUC__)
#define EXPORT_FUNCTION __attribute__ ((visibility("default")))
#else
#define EXPORT_FUNCTION
#endif

#if HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
])

trymemcacheddir=""
AC_ARG_WITH(memcached,
       [  --with-memcached=PATH     Specify path to memcached installation ],
       [
                if test "x$withval" != "xno" ; then
                        trymemcacheddir=$withval
                fi
       ]
)

dnl ----------------------------------------------------------------------------

AC_CACHE_CHECK([for memcached directory], ac_cv_memcached_dir, [
  saved_CPPFLAGS="$CPPFLAGS"
  mc_found=no
  for mcdir in $trymemcacheddir "" $prefix /usr/local ; do
    # Skip the directory if it isn't there.
    if test ! -z "$mcdir" -a ! -d "$mcdir" ; then
       continue;
    fi
    if test ! -z "$mcdir" ; then
      if test -d "$mcdir/include" ; then
        CPPFLAGS="-I$mcdir/include $CPPFLAGS"
      else
        CPPFLAGS="-I$mcdir $CPPFLAGS"
      fi
    fi
    # Can I compile and link it?
    AC_TRY_LINK([#include <memcached/engine.h>], [ ],
       [ memcached_linked=yes ], [ memcached_linked=no ])
    if test $memcached_linked = yes; then
       if test ! -z "$mcdir" ; then
         ac_cv_memcached_dir=$mcdir
       else
         ac_cv_memcached_dir="(system)"
       fi
       mc_found=yes
       break
    fi
  done
  CPPFLAGS="$saved_CPPFLAGS"
  if test $mc_found = no ; then
    AC_MSG_ERROR([memcached is required.

      If it's already installed, specify its path using --with-memcached=/dir/
])
  fi
])
if test $ac_cv_memcached_dir != "(system)"; then
  if test -d "$ac_cv_memcached_dir/lib" ; then
    LDFLAGS="-L$ac_cv_memcached_dir/lib $LDFLAGS"
    mc_libdir="$ac_cv_memcached_dir/lib"
  else
    LDFLAGS="-L$ac_cv_memcached_dir $LDFLAGS"
    mc_libdir="$ac_cv_memcached_dir"
  fi
  if test -d "$ac_cv_memcached_dir/include" ; then
    CPPFLAGS="-I$ac_cv_memcached_dir/include $CPPFLAGS"
  else
    CPPFLAGS="-I$ac_cv_memcached_dir $CPPFLAGS"
  fi
fi

MEMCACHED_DIR=$ac_cv_memcached_dir
AS_IF([test "${MEMCACHED_DIR}" = "(system)"],
      [
      AS_IF([test "x$ac_cv_have_htonll" = "xyes"],[ MEMCACHED_DIR=. ], [
        AC_MSG_ERROR([
ep-engine needs to link with a utility lib from memcached.
Specify its path using --with-memcached. ex: --with-memcached=../memcached
])
        ])
      ])
AC_SUBST(MEMCACHED_DIR)

dnl ----------------------------------------------------------------------------

AC_CONFIG_FILES(Makefile)
AC_OUTPUT
echo "---"
echo "Configuration summary for $PACKAGE_NAME version $VERSION"
echo ""
echo "   * Installation prefix:       $prefix"
echo "   * System type:               $host_vendor-$host_os"
echo "   * Host CPU:                  $host_cpu"
echo "   * C Compiler:                $CC_VERSION"
echo "   * Assertions enabled:        $ac_cv_assert"
echo "   * Debug enabled:             $with_debug"
echo "   * Warnings as failure:       $ac_cv_warnings_as_errors"
echo ""
echo "---"
