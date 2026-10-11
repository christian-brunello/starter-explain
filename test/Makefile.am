#
# starter-explain - test/Makefile.am
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

AM_CFLAGS = $(ST_EXPLAIN_CFLAGS) -I$(top_srcdir)/src -I$(top_srcdir)/test
AM_LDFLAGS = $(ST_EXPLAIN_LIBS)

TESTS = test-explain
check_PROGRAMS = $(TESTS)

SRC_OBJS = \
	$(top_builddir)/src/starter_explain-log.o \
	$(top_builddir)/src/starter_explain-stex-config.o \
	$(top_builddir)/src/starter_explain-state.o \
	$(top_builddir)/src/starter_explain-prompt.o \
	$(top_builddir)/src/starter_explain-debug-client.o \
	$(top_builddir)/src/starter_explain-http-queue.o \
	$(top_builddir)/src/starter_explain-llm.o \
	$(top_builddir)/src/starter_explain-tts.o \
	$(top_builddir)/src/starter_explain-wyoming.o \
	$(top_builddir)/src/starter_explain-audio.o \
	$(top_builddir)/src/starter_explain-explain.o

test_explain_SOURCES = test-explain.c mock-debug-server.c mock-debug-server.h
test_explain_CFLAGS = $(AM_CFLAGS)
test_explain_LDADD = $(SRC_OBJS) $(AM_LDFLAGS)
test_explain_DEPENDENCIES = $(SRC_OBJS)

$(SRC_OBJS):
	$(MAKE) -C $(top_builddir)/src
