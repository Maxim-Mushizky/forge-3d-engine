#pragma once

#include <string>

struct GLFWwindow;

namespace forge {

// Native Win32 open-file dialog. Returns empty string on cancel.
// filter format: "Display Name\0*.ext;*.ext2\0" (Win32 double-NUL convention).
std::string OpenFileDialog(GLFWwindow* owner, const char* filter);

} // namespace forge
