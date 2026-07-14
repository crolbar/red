#include <drm/drm_fourcc.h>
#include <errno.h> // IWYU pragma: keep
#include <libinput.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <gbm.h>

#include "drm.h"
#include "input.h"
#include "log.h"
#include "signals.h"
#include "vt.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

int
main(int argc, char** argv)
{
    ROG_INIT();
    int ret = 0;

    struct drmstate* drm;
    drm = malloc(sizeof(*drm));
    drm->fd = -1;

    struct redstate* rs;
    rs = malloc(sizeof(*rs));
    rs->drm = drm;
    rs->sig_fd = -1;
    rs->tty_fd = -1;
    rs->li = NULL;
    rs->active = 1;
    rs->should_quit = 0;

    // signals
    {
        int signal_fd = init_signals();
        if (signal_fd < 0) {
            ret = -1;
            goto end;
        }
        rs->sig_fd = signal_fd;
    }

    // setup VT
    int vt_enabled;
    {
        int tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (tty_fd < 0) {
            ROG_ERR("open tty: %s", strerror(errno));
            ret = 1;
            goto end;
        }

        if (vt_start(tty_fd) == -1) {
            ret = 1;
            goto end;
        }
        vt_enabled = 1;
        rs->tty_fd = tty_fd;
    }

    // libinput
    {
        struct libinput* li = init_input();
        if (!li) {
            ROG_ERR("failed to init libinput");
            ret = 1;
            goto end;
        }
        rs->li = li;
    }

    // drm device
    ROG_INFO("Opening /dev/dri/card0, first connected connector..");
    {
        int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            ROG_ERR("failed oppening drm device: %s", strerror(errno));
            ret = 1;
            goto end;
        }
        rs->drm->fd = fd;
    }

    {
        drmVersion* ver = drmGetVersion(drm->fd);
        if (!ver) {
            ROG_ERR("drmGetVersion failed");
            ret = 1;
            goto end;
        }
        ROG_INFO("Using Driver: %s", ver->name);
        drmFreeVersion(ver);
    }

    {
        drmModeResPtr res = drmModeGetResources(drm->fd);
        if (!res) {
            ret = 1;
            goto end;
        }

        drmModeConnector* conn = NULL;

        // get first found connected connector
        for (int i = 0; i < res->count_connectors; i++) {
            drmModeConnector* _conn =
              drmModeGetConnector(drm->fd, res->connectors[i]);

            if (!_conn || _conn->connection != DRM_MODE_CONNECTED) {
                drmModeFreeConnector(_conn);
                continue;
            }

            conn = _conn;
            break;
        }

        drmModeEncoder* encoder = drmModeGetEncoder(drm->fd, conn->encoder_id);
        drm->crtc_id = encoder->crtc_id;
        drmModeFreeEncoder(encoder);

        drm->conn_id = conn->connector_id;

        drm->mode = conn->modes[0];

        drm->width = conn->modes[0].hdisplay;
        drm->height = conn->modes[0].vdisplay;
        ROG_INFO("Rendering at: %dx%d", drm->width, drm->height);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
    }

    // gbm device
    {
        drm->gbm_dev = gbm_create_device(drm->fd);
        if (!drm->gbm_dev) {
            ROG_ERR("failed to create gbm device");
            ret = 1;
            goto end;
        }
    }
    ROG("created gbm device");

    // gbm bo
    {
        drm->gbm_bo = gbm_bo_create(drm->gbm_dev,
                                    drm->width,
                                    drm->height,
                                    GBM_FORMAT_XRGB8888,
                                    GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        if (!drm->gbm_bo) {
            ROG_ERR("failed to create bo");
            ret = 1;
            goto end;
        }
    }
    ROG("created gbm bo");

    // egl display
    {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
          (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
            "eglGetPlatformDisplayEXT");

        if (!get_platform_display) {
            ROG_ERR("did not found proc address of getPlatformDisplay");
            ret = 1;
            goto end;
        }

        drm->egl_display =
          get_platform_display(EGL_PLATFORM_GBM_KHR, drm->gbm_dev, NULL);
        if (drm->egl_display == EGL_NO_DISPLAY) {
            ROG_ERR("failed to get egl display: %x", eglGetError());
            ret = 1;
            goto end;
        }

        EGLint major, minor;
        if (!eglInitialize(drm->egl_display, &major, &minor)) {
            ROG_ERR("failed to init egl: %x", eglGetError());
            ret = 1;
            goto end;
        }
    }
    ROG("created egl display");

    // egl context
    {
        // TODO broken?
        EGLint attrs[] = {
            EGL_CONTEXT_MAJOR_VERSION,
            2,
            // EGL_CONTEXT_MINOR_VERSION,
            // 2,
            EGL_NONE,
        };

        drm->egl_context = eglCreateContext(
          drm->egl_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
        if (drm->egl_context == EGL_NO_CONTEXT) {
            ROG_ERR("failed to create egl context: %x", eglGetError());
            ret = 1;
            goto end;
        }

        if (!eglMakeCurrent(drm->egl_display,
                            EGL_NO_SURFACE,
                            EGL_NO_SURFACE,
                            drm->egl_context)) {
            ROG_ERR("eglMakeCurrent failed: %x", eglGetError());
            ret = 1;
            goto end;
        }

        ROG("GL_VERSION: %s", glGetString(GL_VERSION));
    }
    ROG("created egl context");

    // egl image
    EGLImageKHR image;
    {
        int fd = gbm_bo_get_fd(drm->gbm_bo);
        uint32_t stride = gbm_bo_get_stride(drm->gbm_bo);
        uint32_t offset = gbm_bo_get_offset(drm->gbm_bo, 0);
        uint64_t modifier = gbm_bo_get_modifier(drm->gbm_bo);
        uint32_t width = gbm_bo_get_width(drm->gbm_bo);
        uint32_t height = gbm_bo_get_height(drm->gbm_bo);
        uint32_t format = gbm_bo_get_format(drm->gbm_bo);

        ROG("creating image w: %d, h: %d", width, height);

        // TODO modifiers
        EGLint attribs[] = { EGL_WIDTH,
                             width,
                             EGL_HEIGHT,
                             height,
                             EGL_LINUX_DRM_FOURCC_EXT,
                             format,
                             EGL_DMA_BUF_PLANE0_FD_EXT,
                             fd,
                             EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                             offset,
                             EGL_DMA_BUF_PLANE0_PITCH_EXT,
                             stride,
                             EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                             (EGLint)(modifier & 0xFFFFFFFF),
                             EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                             (EGLint)(modifier >> 32),
                             EGL_NONE };

        PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR =
          (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");

        if (!eglCreateImageKHR) {
            ROG_ERR("did not found proc address of eglCreateImageKHR");
            ret = 1;
            goto end;
        }

        image = eglCreateImageKHR(drm->egl_display,
                                  EGL_NO_CONTEXT,
                                  EGL_LINUX_DMA_BUF_EXT,
                                  NULL,
                                  attribs);
        if (image == EGL_NO_IMAGE_KHR) {
            ROG_ERR("failed to create egl image: %x", eglGetError());
            ret = 1;
            goto end;
        }
    }
    ROG("created egl image");

    // gl render buffer
    GLuint renderbuffer;
    {
        glGenRenderbuffers(1, &renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);

        PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
        glEGLImageTargetRenderbufferStorageOES =
          (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress(
            "glEGLImageTargetRenderbufferStorageOES");

        if (!glEGLImageTargetRenderbufferStorageOES) {
            ROG_ERR("did not found proc address of "
                    "glEGLImageTargetRenderbufferStorageOES");
            ret = 1;
            goto end;
        }

        glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
                                               (GLeglImageOES)image);

        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }

    // gl frame buffer
    GLuint framebuffer;
    {
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

        glFramebufferRenderbuffer(
          GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            ROG_ERR("glCheckFramebufferStatus failed: %x, status: %x",
                    glGetError(),
                    status);
            ret = 1;
            goto end;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    ROG("created render && frame buffers")

    // draw
    {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, drm->width, drm->height);
        glClearColor(0x66 / 255.0f, 0x22 / 255.0f, 0x22 / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glFinish();
    }

    {
        uint32_t handles[4] = { 0 };
        uint32_t pitches[4] = { 0 };
        uint32_t offsets[4] = { 0 };
        // uint64_t modifiers[4] = { 0 };

        handles[0] = gbm_bo_get_handle(drm->gbm_bo).u32;
        pitches[0] = gbm_bo_get_stride(drm->gbm_bo);
        offsets[0] = gbm_bo_get_offset(drm->gbm_bo, 0);
        // modifiers[0] = gbm_bo_get_modifier(drm->gbm_bo);
        uint32_t format = gbm_bo_get_format(drm->gbm_bo);

        // TODO fix this modifiers
        // if (drmModeAddFB2WithModifiers(
        //       drm->fd,
        //       drm->width,
        //       drm->height,
        //       format,
        //       handles,
        //       pitches,
        //       offsets,
        //       modifiers,
        //       &drm->fb_id,
        //       DRM_MODE_FB_MODIFIERS // flag telling the kernel modifiers[] is
        //                             // valid
        //       )) {
        if (drmModeAddFB2(drm->fd,
                          drm->width,
                          drm->height,
                          format,
                          handles,
                          pitches,
                          offsets,
                          &drm->fb_id,
                          0)) {
            ROG_ERR("failed mode add fb");
            ret = 1;
            goto end;
        }
    }

    if (drmModeSetCrtc(drm->fd,
                       drm->crtc_id,
                       drm->fb_id,
                       0,
                       0,
                       &drm->conn_id,
                       1,
                       &drm->mode))
        ROG_ERR("failed set crtc: %s", strerror(errno));

    // loop
    {
        struct pollfd fds[2];

        int li_fd = libinput_get_fd(rs->li);
        if (li_fd < 0) {
            ROG_ERR("failed get libinput fd: %s", strerror(errno));
            ret = 1;
            goto end;
        }
        fds[0].fd = li_fd;
        fds[0].events = POLLIN;
        fds[1].fd = rs->sig_fd;
        fds[1].events = POLLIN;

        ROG_INFO("Starting loop...");

        while (!rs->should_quit) {
            if (poll(fds, 2, -1) == -1) {
                ROG_ERR("poll fds error");
                ret = 1;
                goto end;
            }

            // input event
            if (fds[0].revents & POLLIN) {
                if (input_check_close(rs)) {
                    goto end;
                }
            }

            // signal
            if (fds[1].revents & POLLIN) {
                int prev_active = rs->active;

                if (handle_signal(rs) == -1) {
                    ret = 1;
                    goto end;
                }

                // redraw on aquire
                if (rs->active && prev_active != rs->active) {
                    if (drmModeSetCrtc(drm->fd,
                                       drm->crtc_id,
                                       drm->fb_id,
                                       0,
                                       0,
                                       &drm->conn_id,
                                       1,
                                       &drm->mode))
                        ROG_ERR("failed re-set crtc: %s", strerror(errno));
                }
            }
        }
    }

end:
    ROG_WARN("Closing..");

    if (rs->drm->fd != -1)
        close(rs->drm->fd);

    if (rs->tty_fd != -1 && vt_enabled)
        vt_stop(rs->tty_fd);

    if (rs->sig_fd != -1)
        close(rs->sig_fd);

    if (rs->li)
        libinput_unref(rs->li);

    ROG_PRINT_CLOSE();
    return ret;
}
