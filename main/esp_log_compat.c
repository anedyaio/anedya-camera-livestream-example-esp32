// esp_log() was introduced in ESP-IDF 5.5; libpeer_default.a was compiled against it.
// This stub makes the prebuilt library link on IDF 5.4.
#include <stdarg.h>
#include "esp_log.h"
#include "esp_idf_version.h"

// IDF 5.5 introduced esp_log(level, tag, fmt, ...); we stubbed it so the prebuilt
// libpeer links on 5.4. IDF 6.0 ships a real esp_log() with a different signature
// (esp_log_config_t as first arg), so the stub now collides — only compile it on
// pre-5.5 toolchains.
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
void __attribute__((weak)) esp_log(esp_log_level_t level, const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    esp_log_writev(level, tag, format, args);
    va_end(args);
}
#endif

// Empty on purpose. app_main() calls this so the linker keeps this translation
// unit (and therefore the weak esp_log stub above) in the final binary.
void esp_log_compat_include(void)
{
}
