#include "EditorApp.h"

#include <cstring>

int main(int argc, char** argv)
{
    forge::EditorApp app;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rt") == 0)
            app.SetRayTracing(true);
        else if (std::strcmp(argv[i], "--hdri") == 0 && i + 1 < argc)
            app.LoadHDRIFile(argv[++i]);
        else
            app.ImportModel(argv[i]);
    }
    app.Run();
    return 0;
}
