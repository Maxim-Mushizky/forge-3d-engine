#include "EditorApp.h"

#include <cctype>
#include <cstring>
#include <string>

// Hybrid-graphics laptops route OpenGL to the integrated GPU by default —
// the path tracer would run on the iGPU while the discrete GPU idles. These
// exported symbols are the documented driver hints for the high-performance
// GPU; they must live in the executable, not the static engine lib.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {
bool EndsWithNoCase(const std::string& s, const std::string& suffix)
{
    if (s.size() < suffix.size())
        return false;
    for (size_t i = 0; i < suffix.size(); ++i)
        if (std::tolower((unsigned char)s[s.size() - suffix.size() + i]) !=
            std::tolower((unsigned char)suffix[i]))
            return false;
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    forge::EditorApp app;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rt") == 0)
            app.SetRayTracing(true);
        else if (std::strcmp(argv[i], "--hdri") == 0 && i + 1 < argc)
            app.LoadHDRIFile(argv[++i]);
        else if (EndsWithNoCase(argv[i], ".forge"))
            app.OpenSceneFile(argv[i]);
        else
            app.ImportModel(argv[i]);
    }
    app.Run();
    return 0;
}
