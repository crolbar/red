#include "compositor.h"
#include "log.h"
#include "opengl.h"
#include "red.h"
#include "render.h"
#include "time.h"
#include <GLES3/gl3.h>
#include <unistd.h>

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

    if (_render_cursor_part(rs, screen_width, screen_height, x, y, w, h))
        goto fail;
    if (_render_cursor_part(rs, screen_width, screen_height, x, y, h, w))
        goto fail;

    CALL(glBindVertexArray(0));
    CALL(glUseProgram(0));

    return 0;
fail:
    glBindVertexArray(0);
    glUseProgram(0);
    return 1;
}

int
render_surface(struct redstate*   rs,
               struct redsurface* rsurf,
               uint32_t           screen_width,
               uint32_t           screen_height)
{
    if (!rsurf->current_buffer) {
        // if buff is null but we have tex while shm buf,
        // we can just bind the old texture
        if (rsurf->gl_tex && rsurf->old_rendered_buf_type == 1)
            ;
        else
            return 0;
    }

    CALL(glUseProgram(rs->program));
    if (rsurf->parent) {
        CALL(glEnable(GL_BLEND));
        CALL(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    }

    if (gl_bind_texture_from_surface(rsurf))
        goto fail;
    assert(rsurf->w != 0 && rsurf->h != 0);

    CALL(glUniform2fv(rs->dimentions_loc,
                      3,
                      (GLfloat[]){
                        red_get_rsurf_x(rsurf),
                        red_get_rsurf_y(rsurf),
                        rsurf->w,
                        rsurf->h,
                        screen_width,
                        screen_height,
                      }));

    CALL(glBindVertexArray(rs->vao));

    CALL(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));

    CALL(glBindVertexArray(0));
    CALL(glBindTexture(GL_TEXTURE_2D, 0));
    CALL(glDisable(GL_BLEND));
    CALL(glUseProgram(0));

    return 0;
fail:
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_BLEND);
    glUseProgram(0);
    return 1;
}

int
render_subsurfs(struct redsurface* rsurf,
                uint32_t           screen_width,
                uint32_t           screen_height)
{
    if (rsurf->subsurfs.size == 0)
        return 0;

    dll_for_each(rsurf->subsurfs, v)
    {
        if (render_surface(rsurf->rs, v->val, screen_width, screen_height))
            return 1;

        if (render_subsurfs(v->val, screen_width, screen_height))
            return 1;
    }

    return 0;
}

int
render_frame(struct redstate* rs, struct redbuffer* rb)
{
    assert(rs);
    assert(rb);
    assert(rb->fbo);

    uint32_t width  = rs->backend->get_width(rs->backend->d);
    uint32_t height = rs->backend->get_height(rs->backend->d);
    assert(width > 0 && height > 0);

    CALL(glBindFramebuffer(GL_FRAMEBUFFER, rb->fbo));
    CALL(glViewport(0, 0, width, height));

    // draw blank before stopping rendering
    if (rs->should_draw == 2) {
        CALL(glClearColor(0x04 / 255.0f, 0x04 / 255.0f, 0x04 / 255.0f, 1.0f));
        CALL(glClear(GL_COLOR_BUFFER_BIT));
        rs->should_draw = 0;
    }

    // render toplevel
    else if (rs->focused_rt) {
        struct redsurface* rsurf;
        if (!(rsurf = rs->focused_rt->rsurf))
            return 0;

        if (render_surface(rs, rsurf, width, height))
            goto fail;
        if (render_subsurfs(rsurf, width, height))
            goto fail;
    }

    // render background color
    else {
        CALL(glClearColor(0x66 / 255.0f, 0x22 / 255.0f, 0x22 / 255.0f, 1.0f));
        CALL(glClear(GL_COLOR_BUFFER_BIT));
    }

    if (!rs->using_hardware_cursor)
    // render software cursor
    {
        int size = 16;
        if (render_cursor(
              rs, width, height, rs->cursor_x, rs->cursor_y, size, size / 3))
            goto fail;
    }

    CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    return 0;
fail:
    return 1;
}

void
redraw(struct redstate* rs)
{
    if (!rs->should_draw || !rs->needs_redraw)
        return;

    {
        double now          = time_get_elapsed_sec(rs->time_start);
        double dt           = (now - rs->last_frame_time) * 1000;
        rs->last_frame_time = now;
        rs->frame_latency   = dt;
    }

    redbuffer* rb = rs->backend->pull_buffer(rs->backend->d);
    {
        // this rerender should be triggered by frame done
        // which should happen a whole lot after wl_buffer.release
        if (!rb->free) {
            ROG_WARN("buffer not free in redraw, skipping");
            return;
        }

        if (rb->needs_resize)
            if (rs->backend->resize_buffer(rs->backend->d, rb))
                goto err;
    }

    if (render_frame(rs, rb))
        goto err;

    // create fence here to know when drawing is done
    // also so we know when to release dmabuf
    rs->queued_rb = rb;
    if ((rs->pfds[RFD_REDRAWSYNC].fd = egl_create_sync_fd(
           rs->backend->get_egl_display(rs->backend->d))) == -1)
        goto err;

    rs->needs_redraw = 0;
    return;
    // TODO: better handling of errors here?
err:
    ROG_ERR("error occured while redrawing");
    rs->should_quit = 1;
}

// TODO: can we use `IN_FENCE_FD` instead?
void
redraw_done(struct redstate* rs)
{
    assert(rs->queued_rb);

    // rendering on `rs->queued_rb` is done, push it
    rs->backend->push_buffer(rs, rs->queued_rb);
    rs->queued_rb = NULL;

    close(rs->pfds[RFD_REDRAWSYNC].fd);
    rs->pfds[RFD_REDRAWSYNC].fd = -1;
}

void
request_redraw(struct redstate* rs)
{
    rs->needs_redraw = 1;

    // if our vt is not focused we don't handle rendering
    if (!rs->active)
        return;

    // we already have queued up a redraw and its not done.
    // once its done after the page flip, we will get redraw
    // and this request will be processed
    if (rs->queued_rb != NULL)
        return;

    // if we are not ready - page flip in progress - once we are ready
    // we call redraw, so this redraw request will happen on the next
    // available redraw time
    if (!rs->backend->is_ready_for_frame(rs->backend->d))
        return;

    redraw(rs);
}
