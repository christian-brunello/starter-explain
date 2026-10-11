#
# starter-explain - configure.ac
#
# Copyright (C) 2026 Christian Brunello <brncrs@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
# Process this file with autoconf to produce configure.

AC_INIT([starter-explain], [0.1.0], [startercontroller@gmail.com])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([src/config.h])
AC_CONFIG_MACRO_DIRS([m4])

AM_INIT_AUTOMAKE([-Wall -Werror foreign])

AC_PROG_CC
AM_PROG_AR

LT_INIT

PKG_PROG_PKG_CONFIG

PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 2.44 gobject-2.0 >= 2.44 gio-2.0 >= 2.44])
PKG_CHECK_MODULES([LIBXML2], [libxml-2.0 >= 2.9.0])
PKG_CHECK_MODULES([JSON_GLIB], [json-glib-1.0])

have_libsoup3=no
PKG_CHECK_MODULES([LIBSOUP], [libsoup-3.0],
  [have_libsoup3=yes],
  [PKG_CHECK_MODULES([LIBSOUP], [libsoup-2.4 >= 2.58])])

AM_CONDITIONAL([HAVE_LIBSOUP_3], [test "$have_libsoup3" = "yes"])
AS_IF([test "$have_libsoup3" = "yes"],
  [AC_DEFINE([HAVE_LIBSOUP_3], [1], [Define if building against libsoup 3.x])])

ST_EXPLAIN_CFLAGS="$GLIB_CFLAGS $LIBXML2_CFLAGS $JSON_GLIB_CFLAGS $LIBSOUP_CFLAGS"
ST_EXPLAIN_LIBS="$GLIB_LIBS $LIBXML2_LIBS $JSON_GLIB_LIBS $LIBSOUP_LIBS"

AC_SUBST(ST_EXPLAIN_CFLAGS)
AC_SUBST(ST_EXPLAIN_LIBS)

AC_CONFIG_FILES([
  Makefile
  src/Makefile
  examples/Makefile
  test/Makefile
  doc/Makefile
])

AC_OUTPUT

AS_IF([test "$have_libsoup3" = "yes"],
  [soup_msg="libsoup-3.0"],
  [soup_msg="libsoup-2.4"])
AC_MSG_NOTICE([HTTP clients will use $soup_msg])
