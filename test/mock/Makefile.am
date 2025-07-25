# BlueALSA - Makefile.am
# Copyright (c) 2016-2025 Arkadiusz Bokowy

check_PROGRAMS = \
	bluealsad-mock

BUILT_SOURCES = \
	dbus-ifaces.c \
	dbus-ifaces.h

MOSTLYCLEANFILES = \
	$(BUILT_SOURCES)

bluealsad_mock_SOURCES = \
	../../src/shared/a2dp-codecs.c \
	../../src/shared/ffb.c \
	../../src/shared/log.c \
	../../src/shared/rt.c \
	../../src/a2dp.c \
	../../src/a2dp-sbc.c \
	../../src/at.c \
	../../src/audio.c \
	../../src/ba-adapter.c \
	../../src/ba-config.c \
	../../src/ba-device.c \
	../../src/ba-rfcomm.c \
	../../src/ba-transport.c \
	../../src/ba-transport-pcm.c \
	../../src/bluealsa-dbus.c \
	../../src/bluealsa-iface.c \
	../../src/ba-pcm-client.c \
	../../src/ba-pcm-mix-buffer.c \
	../../src/ba-pcm-multi.c \
	../../src/bluez.c \
	../../src/bluez-iface.c \
	../../src/codec-sbc.c \
	../../src/dbus.c \
	../../src/h2.c \
	../../src/hci.c \
	../../src/hci-usb.c \
	../../src/hfp.c \
	../../src/io.c \
	../../src/rtp.c \
	../../src/sco.c \
	../../src/sco-cvsd.c \
	../../src/storage.c \
	../../src/utils.c \
	dbus-ifaces.c \
	mock-bluealsa.c \
	mock-bluez.c \
	mock-ofono.c \
	mock-upower.c \
	mock.c

bluealsad_mock_CFLAGS = \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/test \
	@AAC_CFLAGS@ \
	@ALSA_CFLAGS@ \
	@APTX_CFLAGS@ \
	@APTX_HD_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@GIO2_CFLAGS@ \
	@GLIB2_CFLAGS@ \
	@LC3_CFLAGS@ \
	@LDAC_ABR_CFLAGS@ \
	@LDAC_DEC_CFLAGS@ \
	@LDAC_ENC_CFLAGS@ \
	@LHDC_DEC_CFLAGS@ \
	@LHDC_ENC_CFLAGS@ \
	@LIBBSD_CFLAGS@ \
	@LIBUNWIND_CFLAGS@ \
	@MP3LAME_CFLAGS@ \
	@MPG123_CFLAGS@ \
	@OPUS_CFLAGS@ \
	@SBC_CFLAGS@ \
	@SPANDSP_CFLAGS@

bluealsad_mock_LDADD = \
	@AAC_LIBS@ \
	@ALSA_LIBS@ \
	@APTX_LIBS@ \
	@APTX_HD_LIBS@ \
	@BLUEZ_LIBS@ \
	@GIO2_LIBS@ \
	@GLIB2_LIBS@ \
	@LC3_LIBS@ \
	@LC3PLUS_LIBS@ \
	@LDAC_ABR_LIBS@ \
	@LDAC_DEC_LIBS@ \
	@LDAC_ENC_LIBS@ \
	@LHDC_DEC_LIBS@ \
	@LHDC_ENC_LIBS@ \
	@LIBUNWIND_LIBS@ \
	@MP3LAME_LIBS@ \
	@MPG123_LIBS@ \
	@OPUS_LIBS@ \
	@SBC_LIBS@ \
	@SPANDSP_LIBS@

if ENABLE_AAC
bluealsad_mock_SOURCES += ../../src/a2dp-aac.c
endif

if ENABLE_APTX
bluealsad_mock_SOURCES += ../../src/a2dp-aptx.c
endif

if ENABLE_APTX_HD
bluealsad_mock_SOURCES += ../../src/a2dp-aptx-hd.c
endif

if ENABLE_APTX_OR_APTX_HD
bluealsad_mock_SOURCES += ../../src/codec-aptx.c
endif

if ENABLE_FASTSTREAM
bluealsad_mock_SOURCES += ../../src/a2dp-faststream.c
endif

if ENABLE_LC3PLUS
bluealsad_mock_SOURCES += ../../src/a2dp-lc3plus.c
endif

if ENABLE_LC3_SWB
bluealsad_mock_SOURCES += \
	../../src/codec-lc3-swb.c \
	../../src/sco-lc3-swb.c
endif

if ENABLE_LDAC
bluealsad_mock_SOURCES += ../../src/a2dp-ldac.c
endif

if ENABLE_LHDC
bluealsad_mock_SOURCES += ../../src/a2dp-lhdc.c
endif

if ENABLE_MIDI
bluealsad_mock_SOURCES += \
	../../src/ble-midi.c \
	../../src/bluez-midi.c \
	../../src/midi.c
endif

if ENABLE_MPEG
bluealsad_mock_SOURCES += ../../src/a2dp-mpeg.c
endif

if ENABLE_MSBC
bluealsad_mock_SOURCES += \
	../../src/codec-msbc.c \
	../../src/sco-msbc.c
endif

if ENABLE_OFONO
bluealsad_mock_SOURCES += \
	../../src/ofono.c \
	../../src/ofono-iface.c
endif

if ENABLE_OPUS
bluealsad_mock_SOURCES += ../../src/a2dp-opus.c
endif

if ENABLE_UPOWER
bluealsad_mock_SOURCES += ../../src/upower.c
endif

.xml.h:
	gdbus-codegen --output $@ \
		--interface-prefix org \
		--c-namespace Mock \
		--c-generate-autocleanup all \
		--c-generate-object-manager \
		--header $<

.xml.c:
	gdbus-codegen --output $@ \
		--interface-prefix org \
		--c-namespace Mock \
		--c-generate-autocleanup all \
		--c-generate-object-manager \
		--body $<
