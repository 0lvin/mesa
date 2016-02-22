AUTOMAKE_OPTIONS = subdir-objects

include $(top_srcdir)/src/gallium/Automake.inc

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

include $(top_srcdir)/install-gallium-links.mk
