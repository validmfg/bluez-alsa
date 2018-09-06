# BlueALSA - Makefile.am
# Copyright (c) 2018 Thierry Bultel

bin_PROGRAMS = test_lib

test_lib_SOURCES=test_lib.c

bluealsalib_LTLIBRARIES = libbluealsa.la

libbluealsa_la_SOURCES = \
	bluealsa.c \
	../shared/ctl-client.c

bluealsalibdir = $(libdir)

AM_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@GIO2_CFLAGS@

AM_LDFLAGS = -module -avoid-version

libbluealsa_la_LIBADD = \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@ \
	@GIO2_LIBS@

LDADD=libbluealsa.la
