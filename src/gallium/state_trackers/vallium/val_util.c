
#include "val_private.h"

void val_printflike(3, 4)
__val_finishme(const char *file, int line, const char *format, ...)
{
   va_list ap;
   char buffer[256];

   va_start(ap, format);
   vsnprintf(buffer, sizeof(buffer), format, ap);
   va_end(ap);

   fprintf(stderr, "%s:%d: FINISHME: %s\n", file, line, buffer);
}

VkResult
__vk_errorf(VkResult error, const char *file, int line, const char *format, ...)
{
   va_list ap;
   char buffer[256];

#define ERROR_CASE(error) case error: error_str = #error; break;

   const char *error_str;
   switch ((int32_t)error) {

   /* Core errors */
   ERROR_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
   ERROR_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
   ERROR_CASE(VK_ERROR_INITIALIZATION_FAILED)
   ERROR_CASE(VK_ERROR_DEVICE_LOST)
   ERROR_CASE(VK_ERROR_MEMORY_MAP_FAILED)
   ERROR_CASE(VK_ERROR_LAYER_NOT_PRESENT)
   ERROR_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
   ERROR_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)

   /* Extension errors */
   ERROR_CASE(VK_ERROR_OUT_OF_DATE_KHR)

   default:
      assert(!"Unknown error");
      error_str = "unknown error";
   }

#undef ERROR_CASE

   if (format) {
      va_start(ap, format);
      vsnprintf(buffer, sizeof(buffer), format, ap);
      va_end(ap);

      fprintf(stderr, "%s:%d: %s (%s)\n", file, line, buffer, error_str);
   } else {
      fprintf(stderr, "%s:%d: %s\n", file, line, error_str);
   }

   return error;
}
