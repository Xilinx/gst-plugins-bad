plugin_LTLIBRARIES = libgstkms.la

libgstkms_la_SOURCES = 			\
	gstkmssink.c				\
	gstkmsutils.c				\
	gstkmsallocator.c			\
	gstkmsbufferpool.c			\
	$(NUL)

libgstkms_la_CFLAGS = 			\
	$(GST_PLUGINS_BASE_CFLAGS) 		\
	$(GST_BASE_CFLAGS) 			\
	$(GST_VIDEO_CFLAGS)			\
	$(GST_ALLOCATORS_CFLAGS)		\
	$(GST_CFLAGS) 				\
	$(KMS_DRM_CFLAGS) 			\
	$(NULL)

if HAVE_HDR_OUTPUT_METADATA
libgstkms_la_CFLAGS += -DHAVE_HDR_OUTPUT_METADATA
endif

if HAVE_GEN_HDR_OUTPUT_METADATA
libgstkms_la_CFLAGS += -DHAVE_GEN_HDR_OUTPUT_METADATA
endif

libgstkms_la_LIBADD = 			\
	$(GST_PLUGINS_BASE_LIBS) 		\
	$(GST_BASE_LIBS) 			\
	$(GST_VIDEO_LIBS)			\
	$(GST_ALLOCATORS_LIBS)			\
	$(GST_LIBS) 				\
	$(KMS_DRM_LIBS) 			\
	$(NULL)

libgstkms_la_LDFLAGS = 			\
	$(GST_PLUGIN_LDFLAGS)			\
	$(NULL)

noinst_HEADERS = 				\
	gstkmssink.h 				\
	gstkmsutils.h				\
	gstkmsallocator.h			\
	gstkmsbufferpool.h			\
	$(NULL)
