#include "wiBacklog.h"
#include "wiHelper.h"
#include "wiAppleHelper.h"
#include "wiVersion.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace wi::helper
{
std::string toUpper(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return out;
}

std::string GetCurrentPath()
{
    std::error_code ec;
    const auto path = std::filesystem::current_path(ec);
    if (ec)
    {
        return ".";
    }
    return path.string();
}

void DebugOut(const std::string& str, DebugLevel level)
{
    const char* level_text = "INFO";
    if (level == DebugLevel::Warning)
    {
        level_text = "WARN";
    }
    else if (level == DebugLevel::Error)
    {
        level_text = "ERR";
    }
    std::fprintf(stderr, "[Wicked %s] %s", level_text, str.c_str());
}

void messageBox(const std::string& msg, const std::string& caption)
{
    std::fprintf(stderr, "[Wicked helper::messageBox] %s: %s\n", caption.c_str(), msg.c_str());
}
}

namespace wi::backlog
{
void post(const char* input, LogLevel level)
{
    (void)level;
    if (input != nullptr)
    {
        std::fprintf(stderr, "[Wicked] %s\n", input);
    }
}

void post(const std::string& input, LogLevel level)
{
    post(input.c_str(), level);
}
}

#if !defined(__APPLE__)
namespace wi::apple
{
void SetMetalLayerToWindow(void* window_handle, void* layer_handle)
{
    (void)window_handle;
    (void)layer_handle;
}
}
#endif

namespace wi::version
{
int GetMajor()
{
    return 0;
}

int GetMinor()
{
    return 0;
}

int GetRevision()
{
    return 0;
}
}
