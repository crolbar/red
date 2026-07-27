#include "log.h"
#include "opengl.h"
#include "red.h"
#include "render.h"
#include "time.h"
#include "wayland.h"
#include <GLES3/gl3.h>

int
render_surface(struct redstate* rs, struct redsurface* rsurf)
{
    struct wl_resource* buffer = rsurf->pending_buffer;
    if (!buffer)
        buffer = rsurf->old_pending_buffer;
    if (!buffer)
        return 1;

    int32_t               src_w;
    int32_t               src_h;
    struct wl_shm_buffer* shm = wl_shm_buffer_get(buffer);
    if (shm) {
        wl_shm_buffer_begin_access(shm);

        uint8_t* src        = wl_shm_buffer_get_data(shm);
        int32_t  src_stride = wl_shm_buffer_get_stride(shm);
        src_w               = wl_shm_buffer_get_width(shm);
        src_h               = wl_shm_buffer_get_height(shm);
        GLenum gl_fmt       = GL_RGBA;

        int32_t x = 0; // offset
        int32_t y = 0;
        int32_t w = src_w;
        int32_t h = src_h;

        // removing csd that has been informed by
        // xdg_surface_set_window_geometry
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

        /* create/reuse a texture on rsurf so we don't alloc every frame */
        if (rs->tex == 0) {
            glGenTextures(1, &rs->tex);
            glBindTexture(GL_TEXTURE_2D, rs->tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, rs->tex);
        }

        /* row length in pixels, since stride may include padding */
        glPixelStorei(GL_UNPACK_ROW_LENGTH, src_stride / 4);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, x);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, y);

        if (rs->tex_w != src_w || rs->tex_h != src_h) {
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         GL_RGBA,
                         w,
                         h,
                         0,
                         gl_fmt,
                         GL_UNSIGNED_BYTE,
                         src);
            rs->tex_w = src_w;
            rs->tex_h = src_h;
        } else {
            glTexSubImage2D(
              GL_TEXTURE_2D, 0, 0, 0, w, h, gl_fmt, GL_UNSIGNED_BYTE, src);
        }

        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        wl_shm_buffer_end_access(shm);
    } else {
        struct dmabuf* dmabuf = red_get_dmabuf(buffer);
        if (!dmabuf) {
            ROG_ERR("not shm or dmabuf buffer");
            return 1;
        }

        src_w = dmabuf->width;
        src_h = dmabuf->height;

        EGLDisplay  egl_display = rs->backend->get_egl_display(rs->backend->d);
        EGLImageKHR img         = init_egl_image(egl_display,
                                         dmabuf->width,
                                         dmabuf->height,
                                         dmabuf->format,
                                         dmabuf->planes_count,
                                         dmabuf->planes);

        if (rs->tex == 0)
            glGenTextures(1, &rs->tex);

        glBindTexture(GL_TEXTURE_2D, rs->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_proc->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);

        rs->tex_w = dmabuf->width;
        rs->tex_h = dmabuf->height;
    }

    glUseProgram(rs->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, rs->tex);
    glUniform1i(rs->texture_loc, 0);

    glBindVertexArray(rs->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);

    if (rsurf->pending_buffer) {
        rsurf->old_pending_buffer = buffer;
        wl_buffer_send_release(buffer);
        rsurf->pending_buffer = NULL;
    }

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

    if (rs->focused_trc && rs->focused_trc->rsurf)
        render_surface(rs, rs->focused_trc->rsurf);

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
