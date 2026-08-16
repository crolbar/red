#include "log.h"
#include "opengl.h"
#include "red.h"
#include "wayland.h"
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <drm/drm_fourcc.h>
#include <string.h>

static const char* vertex_shader_src = "\
#version 300 es\n\
precision highp float;\n\
vec2 anim(float av, vec2 pos) {\n\
    vec2 p = pos;\n\
    mat2 tmat = mat2(1.0, av, av, 1.0);\n\
    p *= tmat;\n\
    p = max(vec2(-1, -1), min(vec2(1, 1), p));\n\
    return p;\n\
}\n\
uniform float av;\n\
uniform vec2 dim[3];\n\
in vec2 pos;\n\
out vec2 v_uv;\n\
void main() {\n\
    vec2 p = pos;\n\
    if (av != 0.0) p = anim(av, p);\n\
    p *= vec2(dim[1][0], dim[1][1]);\n\
    p -= vec2(abs(dim[1][0] - dim[2][0]), abs(dim[1][1] - dim[2][1]));\n\
    p *= vec2(1.0 / dim[2][0], 1.0 / dim[2][1]);\n\
    float x = (dim[0][0] / dim[2][0]) * 2.0;\n\
    float y = (dim[0][1] / dim[2][1]) * 2.0;\n\
    p += vec2(x, y);\n\
    gl_Position = vec4(p, 0.0, 1.0);\n\
    v_uv = (pos + 1.0) / 2.0;\n\
}";

static const char* fragment_shader_src = "\
#version 300 es\n\
precision highp float;\n\
uniform sampler2D u_texture;\n\
in vec2 v_uv;\n\
out vec4 frag_color;\n\
void main(){\n\
   frag_color = texture(u_texture, v_uv);\n\
}";

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
gl_setup_program(struct redstate* rs)
{
    GLuint vs = gl_compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    GLuint fs = gl_compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);

    rs->program = glCreateProgram();
    CALL(glAttachShader(rs->program, vs));
    CALL(glAttachShader(rs->program, fs));
    CALL(glLinkProgram(rs->program));

    CALL(glDetachShader(rs->program, vs));
    CALL(glDetachShader(rs->program, fs));
    CALL(glDeleteShader(vs));
    CALL(glDeleteShader(fs));
    GLint ok;
    CALL(glGetProgramiv(rs->program, GL_LINK_STATUS, &ok));
    if (ok == GL_FALSE)
        return 0;

    GLuint pos         = glGetAttribLocation(rs->program, "pos");
    rs->texture_loc    = glGetAttribLocation(rs->program, "u_texture");
    rs->dimentions_loc = glGetUniformLocation(rs->program, "dim");
    rs->anim_loc       = glGetUniformLocation(rs->program, "av");

    const float vertices[] = {
        -1, -1, // top left
        1,  -1, // top right
        -1, 1,  // btm left
        1,  1,  // btm right
    };

    CALL(glGenVertexArrays(1, &rs->vao));
    CALL(glBindVertexArray(rs->vao));

    GLuint vbo;
    CALL(glGenBuffers(1, &vbo));
    CALL(glBindBuffer(GL_ARRAY_BUFFER, vbo));
    CALL(glBufferData(
      GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW));

    CALL(glEnableVertexAttribArray(pos));
    CALL(glVertexAttribPointer(
      pos, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), NULL));

    glBindVertexArray(0);

    return 0;
fail:
    return 1;
}

int
gl_surface_texture_map_shm_image(struct redsurface*    rsurf,
                                 struct wl_shm_buffer* shmbuf,
                                 int32_t               x,
                                 int32_t               y,
                                 int32_t               w,
                                 int32_t               h)
{

    uint8_t* data        = wl_shm_buffer_get_data(shmbuf);
    int32_t  data_stride = wl_shm_buffer_get_stride(shmbuf);
    GLenum   gl_fmt      = GL_BGRA_EXT;

    CALL(glPixelStorei(GL_UNPACK_SKIP_ROWS, y));
    CALL(glPixelStorei(GL_UNPACK_SKIP_PIXELS, x));
    CALL(glPixelStorei(GL_UNPACK_ROW_LENGTH, data_stride / 4));

    wl_shm_buffer_begin_access(shmbuf);

    if (rsurf->w != w || rsurf->h != h) {
        CALL(glTexImage2D(
          GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, gl_fmt, GL_UNSIGNED_BYTE, data));
    }
    // dimentions of texture image are the same, just replace data
    else {
        CALL(glTexSubImage2D(
          GL_TEXTURE_2D, 0, 0, 0, w, h, gl_fmt, GL_UNSIGNED_BYTE, data));
    }

    wl_shm_buffer_end_access(shmbuf);
    red_current_buffer_release(rsurf);

    CALL(glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));
    CALL(glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0));
    CALL(glPixelStorei(GL_UNPACK_SKIP_ROWS, 0));
    return 0;
fail:
    return 1;
}

int
gl_surface_texture_map_egl_image(struct redsurface* rsurf,
                                 struct dmabuf*     dmabuf,
                                 int32_t            x,
                                 int32_t            y,
                                 int32_t            w,
                                 int32_t            h)
{

    if (!dmabuf->egl_img)
    // initially create the egl link to the dmabuf
    {
        // TODO: use x, y, w, h
        if (!(dmabuf->egl_img = init_egl_image(
                rsurf->rs->backend->get_egl_display(rsurf->rs->backend->d),
                dmabuf->width,
                dmabuf->height,
                dmabuf->format,
                dmabuf->planes_count,
                dmabuf->planes)))
            goto fail;
    }

    CALL(gl_proc->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, dmabuf->egl_img));

    return 0;
fail:
    return 1;
}

int
gl_surface_texture_map_image(struct redsurface*  rsurf,
                             struct wl_resource* buffer)
{
    CALL(glBindTexture(GL_TEXTURE_2D, rsurf->gl_tex));

    int32_t               src_w  = 0;
    int32_t               src_h  = 0;
    struct wl_shm_buffer* shmbuf = NULL;
    struct dmabuf*        dmabuf = NULL;
    if ((shmbuf = wl_shm_buffer_get(buffer))) {
        src_w                        = wl_shm_buffer_get_width(shmbuf);
        src_h                        = wl_shm_buffer_get_height(shmbuf);
        rsurf->old_rendered_buf_type = 1;
    } else if ((dmabuf = red_get_dmabuf(buffer))) {
        src_w                        = dmabuf->width;
        src_h                        = dmabuf->height;
        rsurf->old_rendered_buf_type = 2;
    } else {
        ROG_ERR("not shm or dmabuf buffer");
        return 1;
    }

    int32_t x = 0;
    int32_t y = 0;
    int32_t w = src_w;
    int32_t h = src_h;

    if (rsurf->xdg_toplevel || rsurf->zwlr_layer_surface) {
        // removing csd that has been informed by
        // xdg_surface_set_window_geometry
        {
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
        }

        // NOTE: clunky fallback, find better solution
        if (!rsurf->buffer_scale_set && rsurf->configured)
        // set buffer scale back to 1 if recived buffer's
        // width and height are not fitting the full screen
        {
            uint32_t width =
              rsurf->rs->backend->get_width(rsurf->rs->backend->d);
            uint32_t height =
              rsurf->rs->backend->get_height(rsurf->rs->backend->d);
            if (w != (int32_t)width && h != (int32_t)height) {
                rsurf->buffer_scale     = 1;
                rsurf->buffer_scale_set = 1;
                if (rsurf->xdg_toplevel)
                    red_send_toplevel_configure(rsurf, 1, 0);
                else if (rsurf->zwlr_layer_surface)
                    red_send_zwlr_layer_configure(rsurf);
            }
        }
    }

    if (shmbuf)
        gl_surface_texture_map_shm_image(rsurf, shmbuf, x, y, w, h);
    if (dmabuf)
        gl_surface_texture_map_egl_image(rsurf, dmabuf, x, y, w, h);

    // using the old rsurf->w/h values in the shm tex image, so set these after
    rsurf->w = w;
    rsurf->h = h;

    return 0;
fail:
    return 1;
}

int
init_surface_texture_from_buffer(struct redsurface*  rsurf,
                                 struct wl_resource* buffer)
{
    CALL(glGenTextures(1, &rsurf->gl_tex));
    CALL(glBindTexture(GL_TEXTURE_2D, rsurf->gl_tex));

    CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

    if (gl_surface_texture_map_image(rsurf, buffer))
        goto fail;

    CALL(glBindTexture(GL_TEXTURE_2D, 0));
    return 0;
fail:
    return 1;
}

int
gl_bind_texture_from_surface(struct redsurface* rsurf)
{
    struct wl_resource* buffer = rsurf->current_buffer;

    // when buffer is null, just bind old tex
    if (buffer != NULL) {
        if (!rsurf->gl_tex)
        // init texture && map buffer to texture
        {
            if (init_surface_texture_from_buffer(rsurf, buffer))
                goto fail;
        } else
        // map new buffer to texture
        {
            if (gl_surface_texture_map_image(rsurf, buffer))
                goto fail;
        }
    } else if (buffer == NULL && !rsurf->gl_tex)
        ROG("gl_bind_texture, called with buffer of NULL, and no old texture")

    // binding to texture unit 0
    CALL(glActiveTexture(GL_TEXTURE0));
    CALL(glBindTexture(GL_TEXTURE_2D, rsurf->gl_tex));
    CALL(glUniform1i(rsurf->rs->texture_loc, 0));
    return 0;
fail:
    return 1;
}

void
gl_destroy_egl_img(EGLDisplay egl_display, EGLImageKHR egl_img)
{
    CALL(gl_proc->eglDestroyImageKHR(egl_display, egl_img));

fail:
    return;
}

void
gl_destroy_surface_texture(struct redsurface* rsurf)
{
    if (!rsurf->gl_tex)
        return;

    CALL(glDeleteTextures(1, &rsurf->gl_tex));

fail:
    return;
}

int
gl_setup_cursor_program(struct redstate* rs)
{
    GLuint vs = gl_compile_shader(GL_VERTEX_SHADER, cursor_vertex_shader_src);
    GLuint fs =
      gl_compile_shader(GL_FRAGMENT_SHADER, cursor_fragment_shader_src);
    if (!vs || !fs)
        goto fail;

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

// pass pointers to egl_display and context
int
init_egl(struct gbm_device* gbm_dev,
         EGLDisplay*        egl_display,
         EGLContext*        egl_context)
{
    EGLint major, minor;
    {
        *egl_display = gl_proc->eglGetPlatformDisplayEXT(
          EGL_PLATFORM_GBM_KHR, gbm_dev, NULL);
        if (*egl_display == EGL_NO_DISPLAY) {
            ROG_ERR("failed to get egl display: %x", eglGetError());
            return 1;
        }

        if (!eglInitialize(*egl_display, &major, &minor)) {
            ROG_ERR("failed to init egl: %x", eglGetError());
            return 1;
        }
    }

    if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
        ROG_ERR("failed to bind opengl es api: %x", eglGetError());
        return 1;
    };

    const char* exts      = eglQueryString(*egl_display, EGL_EXTENSIONS);
    int req_high_priority = strstr(exts, "EGL_IMG_context_priority") != NULL;

    {
        size_t atti = 0;
        EGLint attrs[32];

        if (req_high_priority) {
            attrs[atti++] = EGL_CONTEXT_PRIORITY_LEVEL_IMG;
            attrs[atti++] = EGL_CONTEXT_PRIORITY_HIGH_IMG;
        }

        attrs[atti++] = EGL_CONTEXT_MAJOR_VERSION;
        attrs[atti++] = 3;
        attrs[atti++] = EGL_CONTEXT_MINOR_VERSION;
        attrs[atti++] = 2;

        attrs[atti++] = EGL_NONE;

        *egl_context = eglCreateContext(
          *egl_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
        if (*egl_context == EGL_NO_CONTEXT) {
            ROG_ERR("failed to create egl context: %x", eglGetError());
            return 1;
        }

        if (req_high_priority) {
            EGLint priority = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
            eglQueryContext(*egl_display,
                            *egl_context,
                            EGL_CONTEXT_PRIORITY_LEVEL_IMG,
                            &priority);
            if (priority != EGL_CONTEXT_PRIORITY_HIGH_IMG) {
                ROG_WARN("failed to get high priority egl context");
            }
        }

        if (!eglMakeCurrent(
              *egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, *egl_context)) {
            ROG_ERR("eglMakeCurrent failed: %x", eglGetError());
            return 1;
        }
    }

    ROG_INFO("EGL version %d.%d", major, minor);
    ROG_INFO("GL version: %s", glGetString(GL_VERSION));
    return 0;
}

int
gl_add_fb(EGLImageKHR egl_image, GLuint* fbo, GLuint* rbo)
{
    CALL(glGenRenderbuffers(1, rbo));
    CALL(glBindRenderbuffer(GL_RENDERBUFFER, *rbo));

    // attaching egl image to glRenderbuffer
    CALL(gl_proc->glEGLImageTargetRenderbufferStorageOES(
      GL_RENDERBUFFER, (GLeglImageOES)egl_image));

    CALL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

    CALL(glGenFramebuffers(1, fbo));
    CALL(glBindFramebuffer(GL_FRAMEBUFFER, *fbo));

    // attach render buffer to framebuffer object
    CALL(glFramebufferRenderbuffer(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, *rbo));

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        ROG_ERR("glCheckFramebufferStatus failed: %x, status: %x",
                glGetError(),
                status);
        goto fail;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return 0;
fail:
    return 1;
}

EGLImageKHR
init_egl_image(EGLDisplay           egl_display,
               uint32_t             width,
               uint32_t             height,
               uint32_t             format,
               uint32_t             planes_count,
               struct dmabuf_plane* planes)
{
    assert(egl_display);
    EGLint attribs[50];
    int    a     = 0;
    attribs[a++] = EGL_WIDTH;
    attribs[a++] = width;
    attribs[a++] = EGL_HEIGHT;
    attribs[a++] = height;
    attribs[a++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[a++] = format;

    struct
    {
        EGLint fd;
        EGLint offset;
        EGLint stride;
        EGLint modifier_lo;
        EGLint modifier_hi;
    } attr_names[4] = { { .fd          = EGL_DMA_BUF_PLANE0_FD_EXT,
                          .offset      = EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                          .stride      = EGL_DMA_BUF_PLANE0_PITCH_EXT,
                          .modifier_lo = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                          .modifier_hi = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
                        { .fd          = EGL_DMA_BUF_PLANE1_FD_EXT,
                          .offset      = EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                          .stride      = EGL_DMA_BUF_PLANE1_PITCH_EXT,
                          .modifier_lo = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                          .modifier_hi = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
                        { .fd          = EGL_DMA_BUF_PLANE2_FD_EXT,
                          .offset      = EGL_DMA_BUF_PLANE2_OFFSET_EXT,
                          .stride      = EGL_DMA_BUF_PLANE2_PITCH_EXT,
                          .modifier_lo = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
                          .modifier_hi = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
                        { .fd          = EGL_DMA_BUF_PLANE3_FD_EXT,
                          .offset      = EGL_DMA_BUF_PLANE3_OFFSET_EXT,
                          .stride      = EGL_DMA_BUF_PLANE3_PITCH_EXT,
                          .modifier_lo = EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
                          .modifier_hi = EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT } };

    for (uint32_t i = 0; i < planes_count; i++) {
        attribs[a++] = attr_names[i].fd;
        attribs[a++] = planes[i].fd;
        attribs[a++] = attr_names[i].offset;
        attribs[a++] = planes[i].offset;
        attribs[a++] = attr_names[i].stride;
        attribs[a++] = planes[i].stride;

        int has_mods = (((uint64_t)planes[i].modifier_hi << 32) |
                        planes[i].modifier_lo) != DRM_FORMAT_MOD_INVALID;

        if (has_mods) {
            attribs[a++] = attr_names[i].modifier_lo;
            attribs[a++] = (EGLint)(planes[i].modifier_lo);
            attribs[a++] = attr_names[i].modifier_hi;
            attribs[a++] = (EGLint)(planes[i].modifier_hi);
        }
    }
    attribs[a++] = EGL_NONE;

    EGLImageKHR img = gl_proc->eglCreateImageKHR(
      egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (img == EGL_NO_IMAGE_KHR) {
        ROG_ERR("eglCreateImageKHR failed: (0x%x)\n", eglGetError());
        return NULL;
    }
    return img;
}

int
egl_create_sync_fd(EGLDisplay egl_display)
{
    EGLSyncKHR sync = gl_proc->eglCreateSyncKHR(
      egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (sync == EGL_NO_SYNC_KHR) {
        ROG_ERR("falied egl create sync");
        goto fail;
    }

    glFlush();

    int fd = gl_proc->eglDupNativeFenceFDANDROID(egl_display, sync);
    if (fd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
        ROG_ERR("falied eglDupNativeFenceFDANDROID");
        goto fail;
    }

    gl_proc->eglDestroySyncKHR(egl_display, sync);

    return fd;
fail:
    return -1;
}

__eglMustCastToProperFunctionPointerType
egl_get_proc(char* addr)
{
    __eglMustCastToProperFunctionPointerType proc = eglGetProcAddress(addr);
    if (!proc) {
        ROG_ERR("did not found proc address of %s", addr);
        return NULL;
    }
    return proc;
}

int
init_gl_proc()
{
    gl_proc = calloc(1, sizeof(*gl_proc));
    if (!gl_proc)
        return 1;
    gl_proc->eglCreateImageKHR                      = NULL;
    gl_proc->glEGLImageTargetRenderbufferStorageOES = NULL;
    gl_proc->glEGLImageTargetTexture2DOES           = NULL;
    gl_proc->eglGetPlatformDisplayEXT               = NULL;

    if (!(gl_proc->eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
            egl_get_proc("eglGetPlatformDisplayEXT")))
        goto fail;

    if (!(gl_proc->eglDestroyImageKHR =
            (PFNEGLDESTROYIMAGEKHRPROC)egl_get_proc("eglDestroyImageKHR")))
        goto fail;

    if (!(gl_proc->eglCreateImageKHR =
            (PFNEGLCREATEIMAGEKHRPROC)egl_get_proc("eglCreateImageKHR")))
        goto fail;

    if (!(gl_proc->eglCreateSyncKHR =
            (PFNEGLCREATESYNCKHRPROC)egl_get_proc("eglCreateSyncKHR")))
        goto fail;

    if (!(gl_proc->eglDestroySyncKHR =
            (PFNEGLDESTROYSYNCKHRPROC)egl_get_proc("eglDestroySyncKHR")))
        goto fail;

    if (!(gl_proc->eglDupNativeFenceFDANDROID =
            (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)egl_get_proc(
              "eglDupNativeFenceFDANDROID")))
        goto fail;

    if (!(gl_proc->glEGLImageTargetRenderbufferStorageOES =
            (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)egl_get_proc(
              "glEGLImageTargetRenderbufferStorageOES")))
        goto fail;

    if (!(gl_proc->glEGLImageTargetTexture2DOES =
            (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)egl_get_proc(
              "glEGLImageTargetTexture2DOES")))
        goto fail;

    return 0;
fail:
    free(gl_proc);
    return 1;
}
