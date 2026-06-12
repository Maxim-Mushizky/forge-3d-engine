#pragma once

#include "forge/core/Math.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace forge {

// Compiles a vertex+fragment program from files under the asset directory.
class Shader {
public:
    Shader(const std::string& vertexPath, const std::string& fragmentPath);
    explicit Shader(const std::string& computePath); // compute program
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    void Bind() const;

    void SetInt(const std::string& name, int value);
    void SetFloat(const std::string& name, float value);
    void SetVec3(const std::string& name, const vec3& value);
    void SetMat3(const std::string& name, const mat3& value);
    void SetMat4(const std::string& name, const mat4& value);

private:
    int UniformLocation(const std::string& name);

    uint32_t m_Program = 0;
    std::unordered_map<std::string, int> m_UniformCache;
};

} // namespace forge
