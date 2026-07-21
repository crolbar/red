#include "log.h"
#include "red.h"
#include "render.h"
#include "time.h"
#include <GLES3/gl3.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wayland-server-protocol.h>

static GLuint g_rect_program = 0;
static GLuint g_rect_vao     = 0;
static GLuint g_rect_vbo     = 0;
static GLuint g_rect_ebo     = 0;

static GLuint quad_program  = 0;
static GLuint quad_vao      = 0;
static GLuint quad_vbo      = 0;
static GLint  u_texture_loc = -1;

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
        ROG_ERR("shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
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
      float  left,
      float  right,
      float  bottom,
      float  top,
      float  near,
      float  far)
{
    memset(m, 0, sizeof(float) * 16);
    m[0]  = 2.0f / (right - left);
    m[5]  = 2.0f / (top - bottom);
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
    m[0]  = w;
    m[5]  = h;
    m[10] = 1.0f;
    m[12] = x;
    m[13] = y;
    m[15] = 1.0f;
}

static void
draw_rect(int   viewport_w,
          int   viewport_h,
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

static bool
ensure_quad_resources(void)
{
    if (quad_program != 0)
        return true;

    static const char* vs_src = "#version 300 es\n"
                                "layout(location = 0) in vec2 a_pos;\n"
                                "layout(location = 1) in vec2 a_uv;\n"
                                "out vec2 v_uv;\n"
                                "void main() {\n"
                                "    v_uv = a_uv;\n"
                                "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
                                "}\n";

    static const char* fs_src = "#version 300 es\n"
                                "precision mediump float;\n"
                                "in vec2 v_uv;\n"
                                "out vec4 frag_color;\n"
                                "uniform sampler2D u_texture;\n"
                                "void main() {\n"
                                "    vec4 c = texture(u_texture, v_uv);\n"
                                "    frag_color = c.bgra;\n"
                                "}\n";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return false;

    quad_program = glCreateProgram();
    glAttachShader(quad_program, vs);
    glAttachShader(quad_program, fs);
    glLinkProgram(quad_program);

    GLint linked = 0;
    glGetProgramiv(quad_program, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(quad_program, sizeof(log), NULL, log);
        fprintf(stderr, "shader link error: %s\n", log);
        glDeleteProgram(quad_program);
        quad_program = 0;
        return false;
    }

    u_texture_loc = glGetUniformLocation(quad_program, "u_texture");

    /* fullscreen quad: pos.xy, uv.xy per vertex
       (uv flipped in Y since wl_shm data is top-down, GL tex origin is
       bottom-left) */
    float verts[] = {
        /* pos        uv */
        -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f, 1.0f,  0.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f,
    };

    glGenVertexArrays(1, &quad_vao);
    glGenBuffers(1, &quad_vbo);

    glBindVertexArray(quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
      0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
      1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);

    return true;
}

static void
draw_textured_quad(struct redstate* rs, GLuint tex, int w, int h)
{
    (void)rs;
    (void)w;
    (void)h; /* unused for now; kept for future scaling/positioning */

    if (!ensure_quad_resources())
        return;

    glUseProgram(quad_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(u_texture_loc, 0);

    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glUseProgram(0);
}

int
render_surface(struct redstate* rs, struct redsurface* rsurf)
{
    struct wl_resource* buffer = rsurf->pending_buffer;
    if (!buffer)
        buffer = rsurf->old_pending_buffer;
    if (!buffer)
        return 1;

    struct wl_shm_buffer* shm = wl_shm_buffer_get(buffer);
    if (!shm) {
        ROG_ERR("not shm buffer?");
        return 1;
    }
    wl_shm_buffer_begin_access(shm);

    uint8_t* src        = wl_shm_buffer_get_data(shm);
    int32_t  src_stride = wl_shm_buffer_get_stride(shm);
    int32_t  w          = wl_shm_buffer_get_width(shm);
    int32_t  h          = wl_shm_buffer_get_height(shm);
    // uint32_t fmt        = wl_shm_buffer_get_format(shm);
    GLenum gl_fmt = GL_RGBA;

    /* create/reuse a texture on rsurf so we don't alloc every frame */
    if (rs->tex == 0) {
        glGenTextures(1, &rs->tex);
        glBindTexture(GL_TEXTURE_2D, rs->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, rs->tex);
    }

    /* row length in pixels, since stride may include padding */
    glPixelStorei(GL_UNPACK_ROW_LENGTH, src_stride / 4);

    if (rs->tex_w != w || rs->tex_h != h) {
        glTexImage2D(
          GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, gl_fmt, GL_UNSIGNED_BYTE, src);
        rs->tex_w = w;
        rs->tex_h = h;
    } else {
        glTexSubImage2D(
          GL_TEXTURE_2D, 0, 0, 0, w, h, gl_fmt, GL_UNSIGNED_BYTE, src);
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    wl_shm_buffer_end_access(shm);

    draw_textured_quad(rs, rs->tex, w, h);

    if (rsurf->pending_buffer) {
        rsurf->old_pending_buffer = buffer;
        wl_buffer_send_release(buffer);
        rsurf->pending_buffer = NULL;
    }

    return 0;
}

int
render_frame(struct redstate* rs, struct redbuffer* rb)
{
    assert(rs);
    assert(rb);

    int width  = rs->backend->get_width(rs->backend->d);
    int height = rs->backend->get_height(rs->backend->d);

    glBindFramebuffer(GL_FRAMEBUFFER, rb->fbo);

    glViewport(0, 0, width, height);
    glClearColor(0x66 / 255.0f, 0x22 / 255.0f, 0x22 / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (rs->focused_toplevel && rs->focused_toplevel->configured)
        render_surface(rs, rs->focused_toplevel);

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

// called when page flip is done
void
redraw(struct redstate* rs)
{
    if (!rs->needs_redraw)
        return;

    {
        double now          = time_get_elapsed_sec(rs->time_start);
        double dt           = (now - rs->last_frame_time) * 1000;
        rs->last_frame_time = now;
        rs->frame_latency   = dt;
    }

    redbuffer* rb = rs->backend->pull_buffer(rs->backend->d);
    // this rerender should be triggered by frame done
    // which should happen a whole lot after wl_buffer.release
    assert(rb->free);

    if (rb->needs_resize)
        if (rs->backend->resize_buffer(rs->backend->d, rb)) {
            rs->should_quit = 1;
            return;
        }

    render_frame(rs, rb);

    rs->backend->push_buffer(rs, rb);
    rs->needs_redraw = 0;
}

void
request_redraw(struct redstate* rs)
{
    rs->needs_redraw = 1;
    // if we are not ready - page flip in progress - once we are ready
    // we call redraw, so this redraw request will happen on the next
    // available redraw time
    if (!rs->backend->is_ready_for_frame(rs->backend->d))
        return;

    redraw(rs);
}
