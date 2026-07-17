#include "red.h"
#include "drm.h"
#include "log.h"
#include "render.h"
#include <GLES3/gl3.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wayland-backend-client.h"

static GLuint g_rect_program = 0;
static GLuint g_rect_vao = 0;
static GLuint g_rect_vbo = 0;
static GLuint g_rect_ebo = 0;

static const char* rect_vs_src =
  "#version 320 es\n"
  "layout(location = 0) in vec2 aPos;\n"
  "uniform mat4 uProjection;\n"
  "uniform mat4 uModel;\n"
  "void main() {\n"
  "    gl_Position = uProjection * uModel * vec4(aPos, 0.0, 1.0);\n"
  "}\n";

static const char* rect_fs_src = "#version 320 es\n"
                                 "precision mediump float;\n"
                                 "out vec4 fragColor;\n"
                                 "uniform vec4 uColor;\n"
                                 "void main() {\n"
                                 "    fragColor = uColor;\n"
                                 "}\n";

static GLuint
compile_shader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile error: %s\n", log);
    }
    return shader;
}

static void
init_rect_renderer(void)
{
    if (g_rect_program)
        return;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, rect_vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, rect_fs_src);

    g_rect_program = glCreateProgram();
    glAttachShader(g_rect_program, vs);
    glAttachShader(g_rect_program, fs);
    glLinkProgram(g_rect_program);

    GLint ok = 0;
    glGetProgramiv(g_rect_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(g_rect_program, sizeof(log), NULL, log);
        fprintf(stderr, "program link error: %s\n", log);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    float vertices[] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
    unsigned short indices[] = { 0, 1, 2, 2, 3, 0 };

    glGenVertexArrays(1, &g_rect_vao);
    glGenBuffers(1, &g_rect_vbo);
    glGenBuffers(1, &g_rect_ebo);

    glBindVertexArray(g_rect_vao);

    glBindBuffer(GL_ARRAY_BUFFER, g_rect_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_rect_ebo);
    glBufferData(
      GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(
      0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

static void
ortho(float* m,
      float left,
      float right,
      float bottom,
      float top,
      float near,
      float far)
{
    memset(m, 0, sizeof(float) * 16);
    m[0] = 2.0f / (right - left);
    m[5] = 2.0f / (top - bottom);
    m[10] = -2.0f / (far - near);
    m[12] = -(right + left) / (right - left);
    m[13] = -(top + bottom) / (top - bottom);
    m[14] = -(far + near) / (far - near);
    m[15] = 1.0f;
}

static void
make_model(float* m, float x, float y, float w, float h)
{
    memset(m, 0, sizeof(float) * 16);
    m[0] = w;
    m[5] = h;
    m[10] = 1.0f;
    m[12] = x;
    m[13] = y;
    m[15] = 1.0f;
}

static void
draw_rect(int viewport_w,
          int viewport_h,
          float x,
          float y,
          float w,
          float h,
          float r,
          float g,
          float b,
          float a)
{
    init_rect_renderer();

    glUseProgram(g_rect_program);

    float proj[16];
    ortho(proj, 0, (float)viewport_w, (float)viewport_h, 0, -1, 1);
    glUniformMatrix4fv(
      glGetUniformLocation(g_rect_program, "uProjection"), 1, GL_FALSE, proj);

    float model[16];
    make_model(model, x, y, w, h);
    glUniformMatrix4fv(
      glGetUniformLocation(g_rect_program, "uModel"), 1, GL_FALSE, model);

    glUniform4f(glGetUniformLocation(g_rect_program, "uColor"), r, g, b, a);

    glBindVertexArray(g_rect_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
    glBindVertexArray(0);
}

int
max(int x, int y)
{
    if (x > y)
        return x;
    return y;
}
int
min(int x, int y)
{
    if (x < y)
        return x;
    return y;
}

int
render_frame(struct redstate* rs, struct redbuffer* rb)
{
    assert(rs);
    assert(rb);

    int width = rs->wl->width;
    int height = rs->wl->height;

    glBindFramebuffer(GL_FRAMEBUFFER, rb->fbo);

    glViewport(0, 0, width, height);
    glClearColor(0x66 / 255.0f, 0x22 / 255.0f, 0x22 / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    {
        float rect_w = 240.0f;
        float rect_h = 240.0f;
        float rect_x = rs->rect_x;
        float rect_y = height - rs->rect_y;
        if (rect_x + rect_w >= width)
            rect_x = width - rect_w;

        if (rect_y + rect_h >= height)
            rect_y = height - rect_h;
        draw_rect(width,
                  height,
                  rect_x,
                  rect_y,
                  rect_w,
                  rect_h,
                  1.0f,
                  1.0f,
                  1.0f,
                  1.0f);
    }

    glFinish();
    return 0;
}

void
render_trigger(int wrender_fd)
{
    int n = write(wrender_fd, (uint8_t[]){ RENDER_TRIGGER_FLIP }, 1);
    if (n != 1) {
        ROG_ERR("failed writing to wrender_fd");
    }
}

void
render_triggerI(int wrender_fd)
{
    int n = write(wrender_fd, (uint8_t[]){ RENDER_TRIGGER_INIT }, 1);
    if (n != 1) {
        ROG_ERR("failed writing to wrender_fd");
    }
}

int
should_render_trigger(int rrender_fd)
{
    uint8_t buffer[1];
    int n = read(rrender_fd, buffer, 1);
    if (n != 1) {
        ROG_ERR("failed reading from rrender_fd");
        return 0;
    }
    if (buffer[0] == RENDER_TRIGGER_INIT || buffer[0] == RENDER_TRIGGER_FLIP)
        return buffer[0];
    return 0;
}
