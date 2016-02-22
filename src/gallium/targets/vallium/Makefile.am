include $(top_srcdir)/src/gallium/Automake.inc

AM_CFLAGS = \
	$(GALLIUM_TARGET_CFLAGS)

lib_LTLIBRARIES = lib@VALLIUM_LIBNAME@.la

lib@VALLIUM_LIBNAME@_la_LDFLAGS = \
	$(LLVM_LDFLAGS) \
	$(BSYMBOLIC) \
	-no-undefined \
	-version-number 1:0 \
	$(GC_SECTIONS) \
	$(LD_NO_UNDEFINED)

lib@VALLIUM_LIBNAME@_la_LIBADD = \
	$(top_builddir)/src/gallium/state_trackers/vallium/libvallium.la \
	$(top_builddir)/src/gallium/auxiliary/libgallium.la \
	$(top_builddir)/src/util/libmesautil.la \
	$(VALLIUM_LIBS) \
	$(PTHREAD_LIBS) \
	$(LLVM_LIBS) \
	$(DLOPEN_LIBS)

nodist_EXTRA_lib@VALLIUM_LIBNAME@_la_SOURCES = dummy.cpp
lib@VALLIUM_LIBNAME@_la_SOURCES =

TARGET_DRIVERS =
TARGET_CPPFLAGS =
TARGET_LIB_DEPS =

include $(top_srcdir)/src/gallium/drivers/softpipe/Automake.inc
include $(top_srcdir)/src/gallium/drivers/llvmpipe/Automake.inc

if HAVE_GALLIUM_STATIC_TARGETS

lib@VALLIUM_LIBNAME@_la_SOURCES += target.c
lib@VALLIUM_LIBNAME@_la_CPPFLAGS = $(TARGET_CPPFLAGS)
lib@VALLIUM_LIBNAME@_la_LIBADD += \
	$(top_builddir)/src/gallium/auxiliary/pipe-loader/libpipe_loader_static.la \
	$(GALLIUM_PIPE_LOADER_WINSYS_LIBS) \
	$(TARGET_LIB_DEPS) \
	$(TARGET_RADEON_WINSYS) $(TARGET_RADEON_COMMON)

else # HAVE_GALLIUM_STATIC_TARGETS

lib@VALLIUM_LIBNAME@_la_LIBADD += \
	$(top_builddir)/src/gallium/auxiliary/pipe-loader/libpipe_loader_dynamic.la

endif # HAVE_GALLIUM_STATIC_TARGETS

include $(top_srcdir)/install-gallium-links.mk
