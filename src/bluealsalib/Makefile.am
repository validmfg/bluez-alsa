# BlueALSA - Makefile.am
# Copyright (c) 2018 Thierry Bultel

bin_PROGRAMS = test_lib

test_lib_SOURCES=test_lib.c

bluealsalib_LTLIBRARIES = libbluealsa.la

include_HEADERS = bluealsa.h

libbluealsa_la_SOURCES = \
	bluealsa.c \
	../shared/ctl-client.c

bluealsalibdir = $(libdir)
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA =bluealsalib.pc

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/shared \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@GIO2_CFLAGS@

AM_LDFLAGS = -module -avoid-version

libbluealsa_la_LIBADD = \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@ \
	@GIO2_LIBS@

libbluealsa_la_LDFLAGS = \
	-version-info 0:0:0

LDADD=libbluealsa.la
