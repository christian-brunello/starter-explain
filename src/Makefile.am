#
# starter-explain - src/Makefile.am
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

bin_PROGRAMS = starter-explain

starter_explain_SOURCES = \
	main.c \
	log.c \
	log.h \
	stex-config.c \
	stex-config.h \
	state.c \
	state.h \
	prompt.c \
	prompt.h \
	debug-client.c \
	debug-client.h \
	http-queue.c \
	http-queue.h \
	llm.c \
	llm.h \
	tts.c \
	tts.h \
	wyoming.c \
	wyoming.h \
	audio.c \
	audio.h \
	explain.c \
	explain.h

starter_explain_CPPFLAGS = $(AM_CPPFLAGS) $(ST_EXPLAIN_CFLAGS) -I$(builddir) -I$(srcdir)
starter_explain_LDADD = $(ST_EXPLAIN_LIBS)
starter_explain_CFLAGS = $(AM_CFLAGS)
