#include "EditorApp.h"

#include <cctype>
#include <cstring>
#include <string>

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
