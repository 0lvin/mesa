#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <stdint.h>

#include "util/macros.h"
#include "util/list.h"

/* Pre-declarations needed for WSI entrypoints */
struct wl_surface;
struct wl_display;
typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_visualid_t;
typedef uint32_t xcb_window_t;

#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR

#define VK_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#include "val_entrypoints.h"

#include <assert.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MAX_VBS         32
#define MAX_SETS         8
#define MAX_RTS          8
#define MAX_VIEWPORTS   16
#define MAX_SCISSORS    16
#define MAX_PUSH_CONSTANTS_SIZE 128
#define MAX_DYNAMIC_BUFFERS 16
#define MAX_IMAGES 8
#define MAX_SAMPLES_LOG2 1

#define val_noreturn __attribute__((__noreturn__))
#define val_printflike(a, b) __attribute__((__format__(__printf__, a, b)))

   //   static_assert(sizeof(*src) == sizeof(*dest), "");

#define typed_memcpy(dest, src, count) ({ \
   memcpy((dest), (src), (count) * sizeof(*(src))); \
})

void *val_resolve_entrypoint(uint32_t index);
void *val_lookup_entrypoint(const char *name);

#define VAL_DEFINE_HANDLE_CASTS(__val_type, __VkType)                      \
                                                                           \
   static inline struct __val_type *                                       \
   __val_type ## _from_handle(__VkType _handle)                            \
   {                                                                       \
      return (struct __val_type *) _handle;                                \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __val_type ## _to_handle(struct __val_type *_obj)                       \
   {                                                                       \
      return (__VkType) _obj;                                              \
   }

#define VAL_DEFINE_NONDISP_HANDLE_CASTS(__val_type, __VkType)              \
                                                                           \
   static inline struct __val_type *                                       \
   __val_type ## _from_handle(__VkType _handle)                            \
   {                                                                       \
      return (struct __val_type *)(uintptr_t) _handle;                     \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __val_type ## _to_handle(struct __val_type *_obj)                       \
   {                                                                       \
      return (__VkType)(uintptr_t) _obj;                                   \
   }

#define VAL_FROM_HANDLE(__val_type, __name, __handle) \
   struct __val_type *__name = __val_type ## _from_handle(__handle)

VAL_DEFINE_HANDLE_CASTS(val_device, VkDevice)
VAL_DEFINE_HANDLE_CASTS(val_instance, VkInstance)
VAL_DEFINE_HANDLE_CASTS(val_physical_device, VkPhysicalDevice)
VAL_DEFINE_HANDLE_CASTS(val_queue, VkQueue)

VAL_DEFINE_NONDISP_HANDLE_CASTS(val_image, VkImage)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_image_view, VkImageView);

extern struct val_dispatch_table dtable;

#define ICD_EXPORT PUBLIC

/* Whenever we generate an error, pass it through this function. Useful for
 * debugging, where we can break on it. Only call at error site, not when
 * propagating errors. Might be useful to plug in a stack trace here.
 */

VkResult __vk_errorf(VkResult error, const char *file, int line, const char *format, ...);

#ifdef DEBUG
#define vk_error(error) __vk_errorf(error, __FILE__, __LINE__, NULL);
#define vk_errorf(error, format, ...) __vk_errorf(error, __FILE__, __LINE__, format, ## __VA_ARGS__);
#else
#define vk_error(error) error
#define vk_errorf(error, format, ...) error
#endif

void __val_finishme(const char *file, int line, const char *format, ...)
   val_printflike(3, 4);

#define val_finishme(format, ...) \
   __val_finishme(__FILE__, __LINE__, format, ##__VA_ARGS__);

#define stub_return(v) \
   do { \
      val_finishme("stub %s", __func__); \
      return (v); \
   } while (0)

#define stub() \
   do { \
      val_finishme("stub %s", __func__); \
      return; \
   } while (0)

static inline void *
val_alloc(const VkAllocationCallbacks *alloc,
          size_t size, size_t align,
          VkSystemAllocationScope scope)
{
   return alloc->pfnAllocation(alloc->pUserData, size, align, scope);
}

static inline void *
val_realloc(const VkAllocationCallbacks *alloc,
            void *ptr, size_t size, size_t align,
            VkSystemAllocationScope scope)
{
   return alloc->pfnReallocation(alloc->pUserData, ptr, size, align, scope);
}

static inline void
val_free(const VkAllocationCallbacks *alloc, void *data)
{
   alloc->pfnFree(alloc->pUserData, data);
}

static inline void *
val_alloc2(const VkAllocationCallbacks *parent_alloc,
           const VkAllocationCallbacks *alloc,
           size_t size, size_t align,
           VkSystemAllocationScope scope)
{
   if (alloc)
      return val_alloc(alloc, size, align, scope);
   else
      return val_alloc(parent_alloc, size, align, scope);
}

static inline void
val_free2(const VkAllocationCallbacks *parent_alloc,
          const VkAllocationCallbacks *alloc,
          void *data)
{
   if (alloc)
      val_free(alloc, data);
   else
      val_free(parent_alloc, data);
}

struct val_physical_device {
    VK_LOADER_DATA                              _loader_data;
    struct val_instance *                       instance;
};

struct val_wsi_interaface;
#define VK_ICD_WSI_PLATFORM_MAX 5

struct val_instance {
   VK_LOADER_DATA _loader_data;
   VkAllocationCallbacks alloc;

   uint32_t apiVersion;
   int physicalDeviceCount;
   struct val_physical_device physicalDevice;

   struct val_wsi_interface *                  wsi[VK_ICD_WSI_PLATFORM_MAX];
};

VkResult val_init_wsi(struct val_instance *instance);
void val_finish_wsi(struct val_instance *instance);

struct val_queue {
   VK_LOADER_DATA                              _loader_data;

   struct val_device *                         device;
};


struct val_device {
   VK_LOADER_DATA                              _loader_data;

   VkAllocationCallbacks                       alloc;

   struct val_queue queue;
   struct val_instance *                       instance;
};

void val_device_get_cache_uuid(void *uuid);

struct val_image {
   VkImageType type;
   VkFormat vk_format;
};

struct val_image_view {
   const struct val_image *image; /**< VkImageViewCreateInfo::image */

};

#ifdef __cplusplus
}
#endif
