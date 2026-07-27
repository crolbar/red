#include "log.h"
#include "opengl.h"
#include "red.h"
#include <GLES3/gl3.h>

static const char* cursor_fragment_shader_src = "\
#version 300 es\n\
precision highp float;\n\
uniform vec4 cursor_color;\n\
out vec4 frag_color;\n\
void main(){\n\
   frag_color = cursor_color;\n\
}";

static const char* cursor_vertex_shader_src = "\
#version 300 es\n\
precision highp float;\n\
layout(location = 0) in vec2 a_pos;\n\
uniform vec2 cursor_coord;\n\
uniform vec2 scale;\n\
uniform vec2 size;\n\
uniform mat2 rotation;\n\
void main() {\n\
    vec2 c_pos = cursor_coord + (scale * (rotation * (a_pos * size)));\n\
    gl_Position = vec4(c_pos, 0.0, 1.0);\n\
}";

static GLuint
gl_compile_shader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        ROG_ERR("shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

int
gl_setup_cursor_program(struct redstate* rs)
{
    GLuint vs = gl_compile_shader(GL_VERTEX_SHADER, cursor_vertex_shader_src);
    GLuint fs =
      gl_compile_shader(GL_FRAGMENT_SHADER, cursor_fragment_shader_src);
    if (!vs || !fs)
        return 0;

    GLuint program = glCreateProgram();

    CALL(glAttachShader(program, vs));
    CALL(glAttachShader(program, fs));

    CALL(glLinkProgram(program));

    CALL(glDeleteShader(vs));
    CALL(glDeleteShader(fs));

    float vertices[] = {
        0, 0, // top-left
        1, 0, // top-right
        1, 1, // bottom-right
        0, 1, // bottom-left
    };
    unsigned short indecies[] = {
        0, 3, 2, 0, 1, 2 //
    };

    // virtex buffer object
    GLuint vbo;
    CALL(glGenBuffers(1, &vbo));

    // index buffer object
    GLuint ibo;
    CALL(glGenBuffers(1, &ibo));

    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    CALL(glBindBuffer(GL_ARRAY_BUFFER, vbo));
    CALL(glBufferData(
      GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW));

    CALL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo));
    CALL(glBufferData(
      GL_ELEMENT_ARRAY_BUFFER, sizeof(indecies), indecies, GL_STATIC_DRAW));

    CALL(glVertexAttribPointer(0,
                               2, // vec2
                               GL_FLOAT,
                               GL_FALSE,
                               2 * sizeof(float), // size of vec
                               (void*)0));

    CALL(glEnableVertexAttribArray(0));
    rs->cursor_gl_program = program;
    rs->cursor_gl_vao     = vao;

    return 0;
fail:
    return 1;
}
