#include "Shader.h"

#include "forge/core/Log.h"

#include <GL/glew.h>

#include <fstream>
#include <sstream>

namespace forge {

static std::string ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    FORGE_ASSERT(file.is_open(), "Cannot open shader file: %s", path.c_str());
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static uint32_t CompileStage(GLenum type, const std::string& source, const std::string& path)
{
    uint32_t shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        FORGE_ERROR("Shader compile failed (%s):\n%s", path.c_str(), log);
        FORGE_ASSERT(false, "shader compile error");
    }
    return shader;
}

Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath)
{
    uint32_t vs = CompileStage(GL_VERTEX_SHADER, ReadFile(vertexPath), vertexPath);
    uint32_t fs = CompileStage(GL_FRAGMENT_SHADER, ReadFile(fragmentPath), fragmentPath);

    m_Program = glCreateProgram();
    glAttachShader(m_Program, vs);
    glAttachShader(m_Program, fs);
    glLinkProgram(m_Program);

    int ok = 0;
    glGetProgramiv(m_Program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(m_Program, sizeof(log), nullptr, log);
        FORGE_ERROR("Program link failed (%s + %s):\n%s", vertexPath.c_str(), fragmentPath.c_str(), log);
        FORGE_ASSERT(false, "shader link error");
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
}

Shader::Shader(const std::string& computePath)
{
    uint32_t cs = CompileStage(GL_COMPUTE_SHADER, ReadFile(computePath), computePath);

    m_Program = glCreateProgram();
    glAttachShader(m_Program, cs);
    glLinkProgram(m_Program);

    int ok = 0;
    glGetProgramiv(m_Program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(m_Program, sizeof(log), nullptr, log);
        FORGE_ERROR("Compute program link failed (%s):\n%s", computePath.c_str(), log);
        FORGE_ASSERT(false, "compute shader link error");
    }
    glDeleteShader(cs);
}

Shader::~Shader() { glDeleteProgram(m_Program); }

void Shader::Bind() const { glUseProgram(m_Program); }

int Shader::UniformLocation(const std::string& name)
{
    auto it = m_UniformCache.find(name);
    if (it != m_UniformCache.end())
        return it->second;
    int loc = glGetUniformLocation(m_Program, name.c_str());
    m_UniformCache[name] = loc;
    return loc;
}

void Shader::SetInt(const std::string& name, int value) { glUniform1i(UniformLocation(name), value); }
void Shader::SetFloat(const std::string& name, float value) { glUniform1f(UniformLocation(name), value); }
void Shader::SetVec3(const std::string& name, const vec3& v) { glUniform3fv(UniformLocation(name), 1, &v.x); }
void Shader::SetMat3(const std::string& name, const mat3& m) { glUniformMatrix3fv(UniformLocation(name), 1, GL_FALSE, glm::value_ptr(m)); }
void Shader::SetMat4(const std::string& name, const mat4& m) { glUniformMatrix4fv(UniformLocation(name), 1, GL_FALSE, glm::value_ptr(m)); }

} // namespace forge
