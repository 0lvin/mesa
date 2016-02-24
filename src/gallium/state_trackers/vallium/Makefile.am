
vulkan_includedir = $(includedir)/vulkan

vulkan_include_HEADERS =				\
	$(top_srcdir)/include/vulkan/vk_platform.h	\
	$(top_srcdir)/include/vulkan/vulkan.h

AM_CPPFLAGS = \
	$(VISIBILITY_CFLAGS) \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/gallium/include \
	-I$(top_srcdir)/src/gallium/drivers \
	-I$(top_srcdir)/src/gallium/auxiliary \
	-I$(top_srcdir)/src/gallium/winsys \
	-I$(srcdir)

VALLIUM_SOURCES = \
	val_device.c \
	val_cmd_buffer.c \
	val_descriptor_set.c \
	val_execute.c \
	val_util.c \
	val_image.c \
	val_formats.c \
	val_pass.c \
	val_pipeline.c \
	val_pipeline_cache.c \
	val_sampler.c \
	val_wsi.c \
	val_wsi_x11.c

BUILT_SOURCES =                                         \
	val_entrypoints.h                               \
	val_entrypoints.c

# Used when generating entrypoints to filter out unwanted extensions
VALLIUM_ENTRYPOINT_CPPFLAGS = \
   -I$(top_srcdir)/include/vulkan \
   -DVK_USE_PLATFORM_XCB_KHR \
   -DVK_USE_PLATFORM_WAYLAND_KHR

val_entrypoints.h : anv_entrypoints_gen.py $(vulkan_include_HEADERS)
	$(AM_V_GEN) cat $(vulkan_include_HEADERS) | $(CPP) $(VALLIUM_ENTRYPOINT_CPPFLAGS) - | $(PYTHON2) $< header > $@

val_entrypoints.c : anv_entrypoints_gen.py $(vulkan_include_HEADERS)
	$(AM_V_GEN) cat $(vulkan_include_HEADERS) | $(CPP) $(VALLIUM_ENTRYPOINT_CPPFLAGS) - | $(PYTHON2) $< code > $@

libvallium_la_SOURCES = $(VALLIUM_SOURCES) $(BUILT_SOURCES)

noinst_LTLIBRARIES = libvallium.la

EXTRA_DIST = Doxyfile

