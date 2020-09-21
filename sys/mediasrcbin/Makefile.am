plugin_LTLIBRARIES = libgstmediasrcbin.la

libgstmediasrcbin_la_SOURCES = 			\
	gstmediasrcbin.c			\
	xmedialib.c				\
	$(NUL)

libgstmediasrcbin_la_CFLAGS = 			\
	$(GST_PLUGINS_BASE_CFLAGS) 		\
	$(GST_BASE_CFLAGS) 			\
	$(GST_VIDEO_CFLAGS)			\
	$(GST_CFLAGS) 				\
	$(MEDIASRCBIN_CFLAGS) 			\
	$(NULL)

libgstmediasrcbin_la_LIBADD = 			\
	$(GST_PLUGINS_BASE_LIBS) 		\
	$(GST_BASE_LIBS) 			\
	$(GST_VIDEO_LIBS)			\
	$(GST_LIBS) 				\
	$(MEDIASRCBIN_LIBS) 			\
	$(NULL)

libgstmediasrcbin_la_LDFLAGS = 			\
	$(GST_PLUGIN_LDFLAGS)			\
	$(MEDIASRCBIN_LDFLAGS) 			\
	$(NULL)

noinst_HEADERS = 				\
	gstmediasrcbin.h 			\
	gstmediasrcbinpad.h			\
	xmedialib.h				\
	$(NULL)
