#include <mimalloc.h>

namespace
{
// Force a direct mimalloc API call once during process startup.
// This helps verify linkage/loading and is useful for Windows DLL override mode.
[[maybe_unused]] const int g_wicked_mimalloc_version = mi_version();
}
