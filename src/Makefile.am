# BlueALSA - Makefile.am
# Copyright (c) 2016-2018 Arkadiusz Bokowy

bin_PROGRAMS = bluealsa
SUBDIRS = asound
SUBDIRS += bluealsalib

bluealsa_SOURCES = \
	shared/ffb.c \
	shared/log.c \
	shared/rt.c \
	at.c \
	bluealsa.c \
	bluez.c \
	bluez-a2dp.c \
	bluez-iface.c \
	ctl.c \
	io.c \
	rfcomm.c \
	transport.c \
	utils.c \
	main.c

if ENABLE_OFONO
bluealsa_SOURCES += \
	ofono.c \
	ofono-iface.c
endif

AM_CFLAGS = \
	-I$(top_srcdir)/src/shared \
	@BLUEZ_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@AAC_CFLAGS@ \
	@APTX_CFLAGS@ \
	@LDAC_CFLAGS@ \
	@LDAC_ABR_CFLAGS@ \
	@SBC_CFLAGS@

LDADD = \
	@BLUEZ_LIBS@ \
	@GLIB2_LIBS@ \
	@GIO2_LIBS@ \
	@AAC_LIBS@ \
	@APTX_LIBS@ \
	@LDAC_LIBS@ \
	@LDAC_ABR_LIBS@ \
	@SBC_LIBS@
