#include "backend-drm.h"
#include "backend-wayland.h"
#include "dll.h"
#include "log.h"
#include "opengl.h"
#include "red.h"
#include "render.h"
#include "time.h"
#include <GLES3/gl3.h>
#include <assert.h>
#include <drm/drm_fourcc.h>
#include <stdio.h>
#include <string.h>
#include <wayland-server-protocol.h>

static GLuint quad_program  = 0;
static GLuint quad_vao      = 0;
static GLuint quad_vbo      = 0;
static GLint  u_texture_loc = -1;

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
                                "    frag_color = c;\n"
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
        -1.0f, -1.0f, 0.0f, 0.0f, //
        1.0f,  -1.0f, 1.0f, 0.0f, //
        -1.0f, 1.0f,  0.0f, 1.0f, //
        1.0f,  1.0f,  1.0f, 1.0f,
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

    if (shm) {
        /* ---- existing CPU shm path (unchanged from before) ---- */
        wl_shm_buffer_begin_access(shm);
        uint8_t* src        = wl_shm_buffer_get_data(shm);
        int32_t  src_stride = wl_shm_buffer_get_stride(shm);
        int32_t  src_w      = wl_shm_buffer_get_width(shm);
        int32_t  src_h      = wl_shm_buffer_get_height(shm);
        GLenum   gl_fmt     = GL_RGBA;
        int32_t  x = 0, y = 0, w = src_w, h = src_h;

        if (rsurf->geom_configured) {
            x = rsurf->geom_x;
            y = rsurf->geom_y;
            w = rsurf->geom_width;
            h = rsurf->geom_height;
            if (x < 0)
                x = 0;
            if (y < 0)
                y = 0;
            if (x + w > src_w)
                w = src_w - x;
            if (y + h > src_h)
                h = src_h - y;
            if (w <= 0 || h <= 0) {
                x = 0;
                y = 0;
                w = src_w;
                h = src_h;
            }
        }

        if (rsurf->tex == 0) {
            glGenTextures(1, &rsurf->tex);
            glBindTexture(GL_TEXTURE_2D, rsurf->tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, rsurf->tex);
        }

        glPixelStorei(GL_UNPACK_ROW_LENGTH, src_stride / 4);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, x);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, y);

        if (rsurf->tex_w != src_w || rsurf->tex_h != src_h) {
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         GL_RGBA,
                         w,
                         h,
                         0,
                         gl_fmt,
                         GL_UNSIGNED_BYTE,
                         src);
            rsurf->tex_w = src_w;
            rsurf->tex_h = src_h;
        } else {
            glTexSubImage2D(
              GL_TEXTURE_2D, 0, 0, 0, w, h, gl_fmt, GL_UNSIGNED_BYTE, src);
        }

        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        wl_shm_buffer_end_access(shm);

        draw_textured_quad(rs, rsurf->tex, src_w, src_h);

        if (rsurf->pending_buffer) {
            rsurf->old_pending_buffer = buffer;
            wl_buffer_send_release(
              buffer); /* CPU copy already done, safe immediately */
            rsurf->pending_buffer = NULL;
        }
        return 0;
    }

    // struct backend_wayland* bw = rs->backend->d;
    struct backend_drm* bd = rs->backend->d;

    struct dmabuf_buffer_data* dmabuf = dmabuf_buffer_get(buffer);
    if (!dmabuf) {
        ROG_ERR("not shm or dmabuf buffer?\n");
        return 1;
    }

    /* only (re)import if this is a different wl_buffer resource than last frame
     */
    if (rsurf->tex_buffer != buffer) {
        if (rsurf->egl_image != EGL_NO_IMAGE_KHR) {
            // gl_proc->eglDestroyImageKHR(rs->egl_display, rsurf->egl_image);
            rsurf->egl_image = EGL_NO_IMAGE_KHR;
        }

        EGLint attribs[50];
        int    a     = 0;
        attribs[a++] = EGL_WIDTH;
        attribs[a++] = dmabuf->width;
        attribs[a++] = EGL_HEIGHT;
        attribs[a++] = dmabuf->height;
        attribs[a++] = EGL_LINUX_DRM_FOURCC_EXT;
        attribs[a++] = dmabuf->format;

        static const EGLint fd_attrs[4]    = { EGL_DMA_BUF_PLANE0_FD_EXT,
                                               EGL_DMA_BUF_PLANE1_FD_EXT,
                                               EGL_DMA_BUF_PLANE2_FD_EXT,
                                               EGL_DMA_BUF_PLANE3_FD_EXT };
        static const EGLint off_attrs[4]   = { EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                               EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                               EGL_DMA_BUF_PLANE2_OFFSET_EXT,
                                               EGL_DMA_BUF_PLANE3_OFFSET_EXT };
        static const EGLint pitch_attrs[4] = { EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                               EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                               EGL_DMA_BUF_PLANE2_PITCH_EXT,
                                               EGL_DMA_BUF_PLANE3_PITCH_EXT };
        static const EGLint modlo_attrs[4] = {
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT
        };
        static const EGLint modhi_attrs[4] = {
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT
        };

        for (int i = 0; i < dmabuf->n_planes; i++) {
            attribs[a++] = fd_attrs[i];
            attribs[a++] = dmabuf->planes[i].fd;
            attribs[a++] = off_attrs[i];
            attribs[a++] = dmabuf->planes[i].offset;
            attribs[a++] = pitch_attrs[i];
            attribs[a++] = dmabuf->planes[i].stride;
            if (dmabuf->planes[i].modifier != DRM_FORMAT_MOD_INVALID) {
                attribs[a++] = modlo_attrs[i];
                attribs[a++] =
                  (EGLint)(dmabuf->planes[i].modifier & 0xffffffff);
                attribs[a++] = modhi_attrs[i];
                attribs[a++] = (EGLint)(dmabuf->planes[i].modifier >> 32);
            }
        }
        attribs[a++] = EGL_NONE;

        EGLImageKHR img = gl_proc->eglCreateImageKHR(bd->egl_display,
                                                     EGL_NO_CONTEXT,
                                                     EGL_LINUX_DMA_BUF_EXT,
                                                     NULL,
                                                     attribs);
        if (img == EGL_NO_IMAGE_KHR) {
            ROG_ERR("eglCreateImageKHR failed for dmabuf buffer (0x%x)\n",
                    eglGetError());
            return 1;
        }

        if (rsurf->tex == 0)
            glGenTextures(1, &rsurf->tex);

        glBindTexture(GL_TEXTURE_2D, rsurf->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_proc->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);

        rsurf->egl_image  = img;
        rsurf->tex_w      = dmabuf->width;
        rsurf->tex_h      = dmabuf->height;
        rsurf->tex_buffer = buffer;
    } else {
        glBindTexture(GL_TEXTURE_2D, rsurf->tex);
    }

    draw_textured_quad(rs, rsurf->tex, rsurf->tex_w, rsurf->tex_h);
    // ROG("render pend: %d", rsurf->pending_buffer)

    if (rsurf->pending_buffer) {
        rsurf->old_pending_buffer = buffer;
        rsurf->pending_buffer     = NULL;

        /* GPU draw above is async: fence it and release once the GPU is
           actually done reading, not immediately. */
        // EGLSyncKHR sync =
        //   gl_proc->eglCreateSyncKHR(bd->egl_display, EGL_SYNC_FENCE_KHR,
        //   NULL);
        // glFlush();

        // ROG("pusing buf: %d", buffer)
        // if (sync != EGL_NO_SYNC_KHR) {
        //     dll_push_tail(
        //       rs->pending_releases,
        //       ((struct pending_release){ .buffer = buffer, .sync = sync }))
        // } else
        wl_buffer_send_release(buffer); /* fallback: fence creation failed */
    }
    // ROG("render")
    return 0;
}

// yoinked out of aquamarine
static char* vertex_shader_src = "\
#version 300 es\n\
precision highp float;\n\
uniform mat3 proj;\n\
in vec2 pos;\n\
in vec2 texcoord;\n\
out vec2 v_texcoord;\n\
void main() {\n\
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);\n\
    v_texcoord = texcoord;\n\
}";

int
_render_cursor_part(struct redstate* rs,
                    uint32_t         screen_width,
                    uint32_t         screen_height,
                    uint32_t         x,
                    uint32_t         y,
                    uint32_t         w,
                    uint32_t         h)
{
    // using vertex shader to move to cursor coords
    {
        float x_frac = (float)x / (float)screen_width;
        float y_frac = (float)y / (float)screen_height;

        float x_ndc = x_frac - (1 - x_frac);
        float y_ndc = y_frac - (1 - y_frac);

        float cursor_coord[2];
        cursor_coord[0] = x_ndc;
        cursor_coord[1] = y_ndc;
        CALL(glUniform2fv(
          glGetUniformLocation(rs->cursor_gl_program, "cursor_coord"),
          1,
          cursor_coord));
    }

    float rotation[4];
    {
        float angle = -5.0f * (M_PI / 180.0f);

        rotation[0] = cosf(angle);
        rotation[1] = sinf(angle);
        rotation[2] = -sinf(angle);
        rotation[3] = cosf(angle);
        CALL(glUniformMatrix2fv(
          glGetUniformLocation(rs->cursor_gl_program, "rotation"),
          1,
          GL_FALSE,
          rotation));
    }

    {
        float size[2];
        size[0] = w;
        size[1] = h;
        CALL(glUniform2fv(
          glGetUniformLocation(rs->cursor_gl_program, "size"), 1, size));
    }

    {
        float scale[2];
        // top and left are 0 not -1
        // so we are scaling up twice
        scale[0] = 2.0f / screen_width;
        scale[1] = 2.0f / screen_height;
        CALL(glUniform2fv(
          glGetUniformLocation(rs->cursor_gl_program, "scale"), 1, scale));
    }

    CALL(glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0));

    return 0;
fail:
    return 1;
}

int
render_cursor(struct redstate* rs,
              uint32_t         screen_width,
              uint32_t         screen_height,
              uint32_t         x,
              uint32_t         y,
              uint32_t         w,
              uint32_t         h)
{
    CALL(glUseProgram(rs->cursor_gl_program));
    CALL(glBindVertexArray(rs->cursor_gl_vao));

    glUniform4f(glGetUniformLocation(rs->cursor_gl_program, "cursor_color"),
                0x99 / 255.0f,
                0x22 / 255.0f,
                0x22 / 255.0f,
                1.0f);

    _render_cursor_part(rs, screen_width, screen_height, x, y, w, h);
    _render_cursor_part(rs, screen_width, screen_height, x, y, h, w);
    return 0;
fail:
    return 1;
}

int
render_frame(struct redstate* rs, struct redbuffer* rb)
{
    assert(rs);
    assert(rb);

    uint32_t width  = rs->backend->get_width(rs->backend->d);
    uint32_t height = rs->backend->get_height(rs->backend->d);

    glBindFramebuffer(GL_FRAMEBUFFER, rb->fbo);

    glViewport(0, 0, width, height);
    glClearColor(0x66 / 255.0f, 0x22 / 255.0f, 0x22 / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (rs->focused_trc && rs->focused_trc->rsurf &&
        rs->focused_trc->rsurf->configured) {
        if (render_surface(rs, rs->focused_trc->rsurf))
            return 1;
    }

    // software cursor
    if (!rs->is_wayland_client && !rs->using_hardware_cursor) {
        int size = 16;
        render_cursor(
          rs, width, height, rs->cursor_x, rs->cursor_y, size, size / 3);
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
    // if our vt is not focused we don't handle rendering
    if (!rs->active)
        return;
    // if we are not ready - page flip in progress - once we are ready
    // we call redraw, so this redraw request will happen on the next
    // available redraw time
    if (!rs->backend->is_ready_for_frame(rs->backend->d))
        return;

    redraw(rs);
}
