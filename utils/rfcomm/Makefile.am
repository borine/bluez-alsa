# BlueALSA - Makefile.am
# Copyright (c) 2016-2021 Arkadiusz Bokowy

if ENABLE_RFCOMM

bin_PROGRAMS = bluealsa-rfcomm

bluealsa_rfcomm_SOURCES = \
	../../src/shared/a2dp-codecs.c \
	../../src/shared/dbus-client.c \
	../../src/shared/dbus-client-rfcomm.c \
	../../src/shared/log.c \
	rfcomm.c

bluealsa_rfcomm_CFLAGS = \
	-I$(top_srcdir)/src \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@ \
	@LIBUNWIND_CFLAGS@ \
	@READLINE_CFLAGS@

bluealsa_rfcomm_LDADD = \
	@DBUS1_LIBS@ \
	@LIBUNWIND_LIBS@ \
	@READLINE_LIBS@

endif
