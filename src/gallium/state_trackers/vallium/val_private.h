#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <assert.h>
#include <stdint.h>

#include "util/macros.h"
#include "util/list.h"

#include "compiler/shader_enums.h"
#include "pipe/p_screen.h"

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

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
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

VAL_DEFINE_HANDLE_CASTS(val_cmd_buffer, VkCommandBuffer)
VAL_DEFINE_HANDLE_CASTS(val_device, VkDevice)
VAL_DEFINE_HANDLE_CASTS(val_instance, VkInstance)
VAL_DEFINE_HANDLE_CASTS(val_physical_device, VkPhysicalDevice)
VAL_DEFINE_HANDLE_CASTS(val_queue, VkQueue)

VAL_DEFINE_NONDISP_HANDLE_CASTS(val_cmd_pool, VkCommandPool)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_buffer, VkBuffer)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_buffer_view, VkBufferView)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_descriptor_set, VkDescriptorSet)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_descriptor_set_layout, VkDescriptorSetLayout)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_device_memory, VkDeviceMemory)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_framebuffer, VkFramebuffer)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_image, VkImage)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_image_view, VkImageView);
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_pipeline_cache, VkPipelineCache)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_pipeline, VkPipeline)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_pipeline_layout, VkPipelineLayout)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_render_pass, VkRenderPass)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_sampler, VkSampler)
VAL_DEFINE_NONDISP_HANDLE_CASTS(val_shader_module, VkShaderModule)
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

struct val_shader_module {
   void *tgsi;
   uint32_t                                     size;
   char                                         data[0];
};

static inline gl_shader_stage
vk_to_mesa_shader_stage(VkShaderStageFlagBits vk_stage)
{
   assert(__builtin_popcount(vk_stage) == 1);
   return ffs(vk_stage) - 1;
}

static inline VkShaderStageFlagBits
mesa_to_vk_shader_stage(gl_shader_stage mesa_stage)
{
   return (1 << mesa_stage);
}

#define VAL_STAGE_MASK ((1 << MESA_SHADER_STAGES) - 1)

#define val_foreach_stage(stage, stage_bits)                         \
   for (gl_shader_stage stage,                                       \
        __tmp = (gl_shader_stage)((stage_bits) & VAL_STAGE_MASK);    \
        stage = __builtin_ffs(__tmp) - 1, __tmp;                     \
        __tmp &= ~(1 << (stage)))
   
struct val_physical_device {
    VK_LOADER_DATA                              _loader_data;
    struct val_instance *                       instance;

    struct pipe_loader_device *pld;
    struct pipe_screen *pscreen;
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

   struct pipe_loader_device *devs;
   int num_devices;
};

VkResult val_init_wsi(struct val_instance *instance);
void val_finish_wsi(struct val_instance *instance);

struct val_queue {
   VK_LOADER_DATA                              _loader_data;

   struct val_device *                         device;
};

struct val_pipeline_cache {
   struct val_device *                          device;
};
struct val_device {
   VK_LOADER_DATA                              _loader_data;

   VkAllocationCallbacks                       alloc;

   struct val_queue queue;
   struct val_instance *                       instance;
   struct val_physical_device *physical_device;
   struct pipe_screen *pscreen;
};

void val_device_get_cache_uuid(void *uuid);

struct val_device_memory {
   struct pipe_memory_allocation *pmem;
   uint32_t                                     type_index;
   VkDeviceSize                                 map_size;
   void *                                       map;
};

struct val_image {
   VkImageType type;
   VkFormat vk_format;
   VkDeviceSize size;
   uint32_t alignment;
   struct pipe_resource *bo;
};

struct val_image_create_info {
   const VkImageCreateInfo *vk_info;
   uint32_t bind_flags;
   uint32_t stride;
};

VkResult
val_image_create(VkDevice _device,
                 const struct val_image_create_info *create_info,
                 const VkAllocationCallbacks* alloc,
                 VkImage *pImage);
   
struct val_image_view {
   const struct val_image *image; /**< VkImageViewCreateInfo::image */

   VkImageViewType view_type;
   VkFormat format;
   VkComponentMapping components;
   VkImageSubresourceRange subresourceRange;

   struct pipe_surface *surface; /* have we created a pipe surface for this? */
};

struct val_subpass {
   uint32_t                                     input_count;
   uint32_t *                                   input_attachments;
   uint32_t                                     color_count;
   uint32_t *                                   color_attachments;
   uint32_t *                                   resolve_attachments;
   uint32_t                                     depth_stencil_attachment;

   /** Subpass has at least one resolve attachment */
   bool                                         has_resolve;
};
   
struct val_render_pass_attachment {
   VkFormat                                     format;
   uint32_t                                     samples;
   VkAttachmentLoadOp                           load_op;
   VkAttachmentLoadOp                           stencil_load_op;
};

struct val_render_pass {
   uint32_t                                     attachment_count;
   uint32_t                                     subpass_count;
   uint32_t *                                   subpass_attachments;
   struct val_render_pass_attachment *          attachments;
   struct val_subpass                           subpasses[0];
};

struct val_sampler {
   uint32_t state[4];
};

struct val_framebuffer {
   uint32_t                                     width;
   uint32_t                                     height;
   uint32_t                                     layers;

   uint32_t                                     attachment_count;
   struct val_image_view *                      attachments[0];
};

struct val_descriptor_set_binding_layout {
   /* Number of array elements in this binding */
   uint16_t array_size;

   /* Index into the flattend descriptor set */
   uint16_t descriptor_index;

   /* Index into the dynamic state array for a dynamic buffer */
   int16_t dynamic_offset_index;

   /* Index into the descriptor set buffer views */
   int16_t buffer_index;

   struct {
      /* Index into the binding table for the associated surface */
      int16_t surface_index;

      /* Index into the sampler table for the associated sampler */
      int16_t sampler_index;

      /* Index into the image table for the associated image */
      int16_t image_index;
   } stage[MESA_SHADER_STAGES];

   /* Immutable samplers (or NULL if no immutable samplers) */
   struct val_sampler **immutable_samplers;
};

struct val_descriptor_set_layout {
   /* Number of bindings in this descriptor set */
   uint16_t binding_count;

   /* Total size of the descriptor set with room for all array entries */
   uint16_t size;

   /* Shader stages affected by this descriptor set */
   uint16_t shader_stages;

   /* Number of buffers in this descriptor set */
   uint16_t buffer_count;

   /* Number of dynamic offsets used by this descriptor set */
   uint16_t dynamic_offset_count;

   /* Bindings in this descriptor set */
   struct val_descriptor_set_binding_layout binding[0];
};

struct val_descriptor {
   VkDescriptorType type;

   union {
      struct {
         struct val_image_view *image_view;
         struct val_sampler *sampler;
      };
      struct val_buffer_view *buffer_view;
   };
};

struct val_descriptor_set {
   const struct val_descriptor_set_layout *layout;
   uint32_t buffer_count;

   struct val_buffer_view *buffer_views;
   struct val_descriptor descriptors[0];
};

VkResult
val_descriptor_set_create(struct val_device *device,
                          const struct val_descriptor_set_layout *layout,
                          struct val_descriptor_set **out_set);

void
val_descriptor_set_destroy(struct val_device *device,
                           struct val_descriptor_set *set);
   
struct val_pipeline_layout {
   struct {
      struct val_descriptor_set_layout *layout;
      uint32_t dynamic_offset_start;
   } set[MAX_SETS];

   uint32_t num_sets;

   struct {
      bool has_dynamic_offsets;
   } stage[MESA_SHADER_STAGES];
};

struct val_pipeline {
   struct val_device *                          device;
   struct val_pipeline_layout *                 layout;
   VkGraphicsPipelineCreateInfo create_info;
};

struct val_buffer {
   struct val_device *                          device;
   VkDeviceSize                                 size;

   VkBufferUsageFlags                           usage;
   VkDeviceSize                                 offset;

   struct pipe_resource *bo;
   uint64_t total_size;
};

struct val_buffer_view {
   VkFormat format;
   struct pipe_resource *bo;
   uint32_t offset;
   uint64_t range;
};

struct val_cmd_pool {
   VkAllocationCallbacks                        alloc;
   struct list_head                             cmd_buffers;
};

#define VAL_CMD_BIND_PIPELINE 1
#define VAL_CMD_BIND_VERTEX_BUFFERS 2
#define VAL_CMD_BIND_DESCRIPTOR_SETS 3
#define VAL_CMD_BEGIN_RENDER_PASS 4
#define VAL_CMD_END_RENDER_PASS 5   
#define VAL_CMD_DRAW 6

struct val_cmd_bind_pipeline {
   VkPipelineBindPoint bind_point;
   struct val_pipeline *pipeline;
};

struct val_cmd_bind_vertex_buffers {
   uint32_t first;
   uint32_t binding_count;
   const struct val_buffer **buffers;
   const VkDeviceSize *offsets;
};

struct val_cmd_bind_descriptor_sets {
   VkPipelineBindPoint bind_point;
   struct val_pipeline_layout *layout;
   uint32_t first;
   uint32_t count;
   const struct val_descriptor_set **sets;
   uint32_t dynamic_offset_count;
   const uint32_t *dynamic_offsets;
};

struct val_cmd_begin_render_pass {
   struct val_framebuffer *framebuffer;
   struct val_render_pass *render_pass;
   VkRect2D render_area;
   uint32_t clear_value_count;
   const VkClearValue *clear_values;
};

struct val_cmd_draw {
   uint32_t vertex_count;
   uint32_t instance_count;
   uint32_t first_vertex;
   uint32_t first_instance;
};

struct val_cmd_buffer_entry {
   struct list_head cmd_link;
   uint32_t cmd_type;
   union {
      struct val_cmd_bind_pipeline pipeline;
      struct val_cmd_bind_vertex_buffers vertex_buffers;
      struct val_cmd_bind_descriptor_sets descriptor_sets;
      struct val_cmd_begin_render_pass begin_render_pass;
      struct val_cmd_draw draw;
   } u;
};
      
struct val_cmd_buffer {
   VK_LOADER_DATA                               _loader_data;

   struct val_device *                          device;

   struct val_cmd_pool *                        pool;
   struct list_head                             pool_link;

   struct list_head                             cmds;
};

VkResult val_execute_cmds(struct val_device *device,
                          struct val_cmd_buffer *cmd_buffer);

enum pipe_format vk_format_to_pipe(VkFormat format);
#ifdef __cplusplus
}
#endif
