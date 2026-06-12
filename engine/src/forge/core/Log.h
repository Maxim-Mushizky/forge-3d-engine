#pragma once

#include <cstdio>
#include <cstdlib>

// Minimal leveled logging. Will grow a ring buffer for the editor console panel (M6).
#define FORGE_INFO(...)  do { std::printf("[info]  " __VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while (0)
#define FORGE_WARN(...)  do { std::printf("[warn]  " __VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while (0)
#define FORGE_ERROR(...) do { std::fprintf(stderr, "[error] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)

#define FORGE_ASSERT(cond, ...)                                                  \
    do {                                                                         \
        if (!(cond)) {                                                           \
            FORGE_ERROR("Assertion failed: %s — " #cond, __VA_ARGS__);           \
            std::abort();                                                        \
        }                                                                        \
    } while (0)
