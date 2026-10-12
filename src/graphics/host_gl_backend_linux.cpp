#if defined(__linux__)

#include "host_gl_backend.h"

#include "../emulator.h"

#include <EGL/egl.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using GlBindFramebufferFn = void (*)(unsigned int, unsigned int);
using GlClearFn = void (*)(unsigned int);
using GlClearColorFn = void (*)(float, float, float, float);
using GlDisableFn = void (*)(unsigned int);
using GlEnableFn = void (*)(unsigned int);
using GlGetFloatvFn = void (*)(unsigned int, float*);
using GlGetIntegervFn = void (*)(unsigned int, int*);
using GlIsEnabledFn = unsigned char (*)(unsigned int);
using GlScissorFn = void (*)(int, int, int, int);
using GlViewportFn = void (*)(int, int, int, int);

GlBindFramebufferFn g_real_gl_bind_framebuffer = nullptr;
GlClearFn g_real_gl_clear = nullptr;
GlClearColorFn g_real_gl_clear_color = nullptr;
GlDisableFn g_real_gl_disable = nullptr;
GlEnableFn g_real_gl_enable = nullptr;
GlGetFloatvFn g_real_gl_get_floatv = nullptr;
GlGetIntegervFn g_real_gl_get_integerv = nullptr;
GlIsEnabledFn g_real_gl_is_enabled = nullptr;
GlScissorFn g_real_gl_scissor = nullptr;
GlViewportFn g_real_gl_viewport = nullptr;
void* g_gles_library = nullptr;

unsigned int g_bound_draw_framebuffer = 0;
int g_content_width = 960;
int g_content_height = 640;
int g_letterbox_x = 0;
int g_letterbox_y = 0;
int g_letterbox_width = 960;
int g_letterbox_height = 640;

void* LookupGlProc(const char* name) {
    if (void* proc = reinterpret_cast<void*>(eglGetProcAddress(name)); proc != nullptr) {
        return proc;
    }
    if (g_gles_library != nullptr) {
        if (void* proc = dlsym(g_gles_library, name); proc != nullptr) {
            return proc;
        }
    }
    return dlsym(RTLD_DEFAULT, name);
}

template<typename Proc>
void EnsureRealProc(Proc& proc, const char* name, const char* fallback = nullptr) {
    if (proc != nullptr) {
        return;
    }
    proc = reinterpret_cast<Proc>(LookupGlProc(name));
    if (proc == nullptr && fallback != nullptr) {
        proc = reinterpret_cast<Proc>(LookupGlProc(fallback));
    }
}

void TransformDefaultFramebufferRect(const int x, const int y, const int width, const int height,
                                     int& out_x, int& out_y, int& out_width, int& out_height) {
    const float scale_x = static_cast<float>(g_letterbox_width) / static_cast<float>(g_content_width);
    const float scale_y = static_cast<float>(g_letterbox_height) / static_cast<float>(g_content_height);
    out_x = g_letterbox_x + static_cast<int>(std::lround(static_cast<float>(x) * scale_x));
    out_y = g_letterbox_y + static_cast<int>(std::lround(static_cast<float>(y) * scale_y));
    out_width = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale_x)));
    out_height = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale_y)));
}

void LinuxGlBindFramebuffer(const unsigned int target, const unsigned int framebuffer) {
    constexpr unsigned int kGlFramebuffer = 0x8D40;
    constexpr unsigned int kGlDrawFramebuffer = 0x8CA9;
    if (target == kGlFramebuffer || target == kGlDrawFramebuffer) {
        g_bound_draw_framebuffer = framebuffer;
    }
    EnsureRealProc(g_real_gl_bind_framebuffer, "glBindFramebuffer", "glBindFramebufferOES");
    if (g_real_gl_bind_framebuffer != nullptr) {
        g_real_gl_bind_framebuffer(target, framebuffer);
    }
}

void LinuxGlScissor(const int x, const int y, const int width, const int height) {
    EnsureRealProc(g_real_gl_scissor, "glScissor");
    if (g_real_gl_scissor == nullptr) {
        return;
    }
    if (g_bound_draw_framebuffer == 0) {
        int mapped_x = 0;
        int mapped_y = 0;
        int mapped_width = 0;
        int mapped_height = 0;
        TransformDefaultFramebufferRect(x, y, width, height, mapped_x, mapped_y, mapped_width, mapped_height);
        g_real_gl_scissor(mapped_x, mapped_y, mapped_width, mapped_height);
        return;
    }
    g_real_gl_scissor(x, y, width, height);
}

void LinuxGlViewport(const int x, const int y, const int width, const int height) {
    EnsureRealProc(g_real_gl_viewport, "glViewport");
    if (g_real_gl_viewport == nullptr) {
        return;
    }
    if (g_bound_draw_framebuffer == 0) {
        int mapped_x = 0;
        int mapped_y = 0;
        int mapped_width = 0;
        int mapped_height = 0;
        TransformDefaultFramebufferRect(x, y, width, height, mapped_x, mapped_y, mapped_width, mapped_height);
        g_real_gl_viewport(mapped_x, mapped_y, mapped_width, mapped_height);
        return;
    }
    g_real_gl_viewport(x, y, width, height);
}

int GuestPixelsFromRequest(const int value) {
    return value <= 640 ? std::max(1, value * 2) : std::max(1, value);
}

}  // namespace

class LinuxHostGLBackend final : public HostGLBackend {
public:
    LinuxHostGLBackend() {
        display_ = XOpenDisplay(nullptr);
        if (display_ == nullptr) {
            throw std::runtime_error("Unable to open the X11 display; ensure DISPLAY is set and XWayland is available");
        }
        gles_library_ = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
        if (gles_library_ == nullptr) {
            gles_library_ = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (gles_library_ == nullptr) {
            XCloseDisplay(display_);
            display_ = nullptr;
            throw std::runtime_error("Unable to load libGLESv2; install the Mesa OpenGL ES runtime");
        }
        g_gles_library = gles_library_;
    }

    ~LinuxHostGLBackend() override {
        if (display_ != nullptr) {
            if (egl_display_ != EGL_NO_DISPLAY) {
                eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                if (surface_ != EGL_NO_SURFACE) {
                    eglDestroySurface(egl_display_, surface_);
                }
                if (context_ != EGL_NO_CONTEXT) {
                    eglDestroyContext(egl_display_, context_);
                }
                eglTerminate(egl_display_);
            }
            if (window_ != 0) {
                XDestroyWindow(display_, window_);
            }
            if (colormap_ != 0) {
                XFreeColormap(display_, colormap_);
            }
            XCloseDisplay(display_);
        }
        if (gles_library_ != nullptr) {
            dlclose(gles_library_);
            g_gles_library = nullptr;
        }
    }

    bool IsSupported() const override {
        return true;
    }

    void EnsureWindow(const int width, const int height, const std::string& title) override {
        EnsureContext();
        if (window_ == 0) {
            CreateWindow(width, height, title);
        }
        Resize(width, height);
        MakeCurrent();
        if (!reported_context_) {
            reported_context_ = true;
            std::fprintf(stderr, "[host-gl/linux] X11 EGL GLES2 window=%dx%d surface=%dx%d context=%p\n",
                width, height, surface_width_, surface_height_, context_);
        }
    }

    void Resize(const int width, const int height) override {
        content_width_ = GuestPixelsFromRequest(width);
        content_height_ = GuestPixelsFromRequest(height);
        if (window_ != 0 && (width != requested_width_ || height != requested_height_)) {
            requested_width_ = width;
            requested_height_ = height;
            XResizeWindow(display_, window_, static_cast<unsigned int>(std::max(1, width)),
                static_cast<unsigned int>(std::max(1, height)));
            XFlush(display_);
        }
        UpdateSurfaceSize();
    }

    void MakeCurrent() override {
        if (egl_display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE && context_ != EGL_NO_CONTEXT
            && eglMakeCurrent(egl_display_, surface_, surface_, context_) != EGL_TRUE) {
            throw std::runtime_error("eglMakeCurrent failed for the Linux window");
        }
    }

    void Present() override {
        if (egl_display_ == EGL_NO_DISPLAY || surface_ == EGL_NO_SURFACE) {
            return;
        }
        ClearLetterbox();
        if (eglSwapBuffers(egl_display_, surface_) != EGL_TRUE) {
            const EGLint error = eglGetError();
            if (error != EGL_BAD_SURFACE && error != EGL_CONTEXT_LOST) {
                throw std::runtime_error("eglSwapBuffers failed for the Linux window");
            }
        }
    }

    void PumpEvents() override {
        while (XPending(display_) > 0) {
            XEvent event{};
            XNextEvent(display_, &event);
            switch (event.type) {
            case ClientMessage:
                if (static_cast<Atom>(event.xclient.data.l[0]) == wm_delete_window_) {
                    should_close_ = true;
                }
                break;
            case ConfigureNotify:
                surface_width_ = std::max(1, event.xconfigure.width);
                surface_height_ = std::max(1, event.xconfigure.height);
                UpdateLetterbox();
                break;
            case ButtonPress:
                if (event.xbutton.button == Button1 && !should_close_) {
                    mouse_down_ = EnqueueTouch(
                        HostTouchPhase::Began, event.xbutton.x, event.xbutton.y, event.xbutton.time);
                }
                break;
            case MotionNotify:
                if (mouse_down_) {
                    EnqueueTouch(HostTouchPhase::Moved, event.xmotion.x, event.xmotion.y, event.xmotion.time);
                }
                break;
            case ButtonRelease:
                if (event.xbutton.button == Button1 && mouse_down_) {
                    mouse_down_ = false;
                    EnqueueTouch(HostTouchPhase::Ended, event.xbutton.x, event.xbutton.y, event.xbutton.time);
                }
                break;
            case KeyPress:
                EnqueueKey(event.xkey);
                break;
            default:
                break;
            }
        }
    }

    bool ShouldClose() const override {
        return should_close_;
    }

    void* GetProcAddress(const char* name) override {
        if (name == nullptr) {
            return nullptr;
        }
        if (std::strcmp(name, "glBindFramebuffer") == 0 || std::strcmp(name, "glBindFramebufferOES") == 0) {
            EnsureRealProc(g_real_gl_bind_framebuffer, "glBindFramebuffer", "glBindFramebufferOES");
            return reinterpret_cast<void*>(&LinuxGlBindFramebuffer);
        }
        if (std::strcmp(name, "glScissor") == 0) {
            EnsureRealProc(g_real_gl_scissor, "glScissor");
            return reinterpret_cast<void*>(&LinuxGlScissor);
        }
        if (std::strcmp(name, "glViewport") == 0) {
            EnsureRealProc(g_real_gl_viewport, "glViewport");
            return reinterpret_cast<void*>(&LinuxGlViewport);
        }
        if (void* proc = reinterpret_cast<void*>(eglGetProcAddress(name)); proc != nullptr) {
            return proc;
        }
        if (gles_library_ != nullptr) {
            if (void* proc = dlsym(gles_library_, name); proc != nullptr) {
                return proc;
            }
        }
        return dlsym(RTLD_DEFAULT, name);
    }

private:
    void EnsureContext() {
        if (egl_display_ != EGL_NO_DISPLAY) {
            return;
        }
        egl_display_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display_));
        if (egl_display_ == EGL_NO_DISPLAY || eglInitialize(egl_display_, nullptr, nullptr) != EGL_TRUE) {
            throw std::runtime_error("Unable to initialize EGL on the X11 display");
        }
        if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
            throw std::runtime_error("eglBindAPI(EGL_OPENGL_ES_API) failed");
        }
        const EGLint config_attributes[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
            EGL_NONE,
        };
        EGLint config_count = 0;
        if (eglChooseConfig(egl_display_, config_attributes, &config_, 1, &config_count) != EGL_TRUE
            || config_count == 0) {
            throw std::runtime_error("eglChooseConfig could not find an OpenGL ES 2 X11 configuration");
        }
        const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context_ = eglCreateContext(egl_display_, config_, EGL_NO_CONTEXT, context_attributes);
        if (context_ == EGL_NO_CONTEXT) {
            throw std::runtime_error("eglCreateContext failed for OpenGL ES 2");
        }
    }

    void CreateWindow(const int width, const int height, const std::string& title) {
        EGLint visual_id = 0;
        if (eglGetConfigAttrib(egl_display_, config_, EGL_NATIVE_VISUAL_ID, &visual_id) != EGL_TRUE) {
            throw std::runtime_error("eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID) failed");
        }
        XVisualInfo visual_template{};
        visual_template.visualid = static_cast<VisualID>(visual_id);
        int visual_count = 0;
        XVisualInfo* visual_info = XGetVisualInfo(display_, VisualIDMask, &visual_template, &visual_count);
        if (visual_info == nullptr || visual_count == 0) {
            throw std::runtime_error("Unable to find an X11 visual matching the EGL configuration");
        }
        const int screen = visual_info->screen;
        colormap_ = XCreateColormap(display_, RootWindow(display_, screen), visual_info->visual, AllocNone);
        XSetWindowAttributes attributes{};
        attributes.colormap = colormap_;
        attributes.event_mask = ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask
            | PointerMotionMask | KeyPressMask;
        window_ = XCreateWindow(display_, RootWindow(display_, screen), 80, 80,
            static_cast<unsigned int>(std::max(1, width)), static_cast<unsigned int>(std::max(1, height)),
            0, visual_info->depth, InputOutput, visual_info->visual, CWColormap | CWEventMask, &attributes);
        XFree(visual_info);
        if (window_ == 0) {
            throw std::runtime_error("XCreateWindow failed");
        }
        XStoreName(display_, window_, title.c_str());
        wm_delete_window_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display_, window_, &wm_delete_window_, 1);
        XMapWindow(display_, window_);
        XFlush(display_);
        requested_width_ = width;
        requested_height_ = height;
        surface_width_ = std::max(1, width);
        surface_height_ = std::max(1, height);
        surface_ = eglCreateWindowSurface(egl_display_, config_, static_cast<EGLNativeWindowType>(window_), nullptr);
        if (surface_ == EGL_NO_SURFACE) {
            throw std::runtime_error("eglCreateWindowSurface failed for the X11 window");
        }
        UpdateLetterbox();
    }

    void UpdateSurfaceSize() {
        if (window_ == 0) {
            return;
        }
        XWindowAttributes attributes{};
        if (XGetWindowAttributes(display_, window_, &attributes) != 0) {
            surface_width_ = std::max(1, attributes.width);
            surface_height_ = std::max(1, attributes.height);
            UpdateLetterbox();
        }
    }

    void UpdateLetterbox() {
        const float content_aspect = static_cast<float>(content_width_) / static_cast<float>(content_height_);
        const float surface_aspect = static_cast<float>(surface_width_) / static_cast<float>(surface_height_);
        if (surface_aspect > content_aspect) {
            letterbox_height_ = surface_height_;
            letterbox_width_ = std::max(1, static_cast<int>(std::lround(surface_height_ * content_aspect)));
        } else {
            letterbox_width_ = surface_width_;
            letterbox_height_ = std::max(1, static_cast<int>(std::lround(surface_width_ / content_aspect)));
        }
        letterbox_x_ = (surface_width_ - letterbox_width_) / 2;
        letterbox_y_ = (surface_height_ - letterbox_height_) / 2;

        g_content_width = content_width_;
        g_content_height = content_height_;
        g_letterbox_x = letterbox_x_;
        g_letterbox_y = letterbox_y_;
        g_letterbox_width = letterbox_width_;
        g_letterbox_height = letterbox_height_;
    }

    void ClearLetterbox() {
        if (letterbox_x_ == 0 && letterbox_y_ == 0
            && letterbox_width_ == surface_width_ && letterbox_height_ == surface_height_) {
            return;
        }
        EnsureRealProc(g_real_gl_bind_framebuffer, "glBindFramebuffer", "glBindFramebufferOES");
        EnsureRealProc(g_real_gl_clear, "glClear");
        EnsureRealProc(g_real_gl_clear_color, "glClearColor");
        EnsureRealProc(g_real_gl_disable, "glDisable");
        EnsureRealProc(g_real_gl_enable, "glEnable");
        EnsureRealProc(g_real_gl_get_floatv, "glGetFloatv");
        EnsureRealProc(g_real_gl_get_integerv, "glGetIntegerv");
        EnsureRealProc(g_real_gl_is_enabled, "glIsEnabled");
        EnsureRealProc(g_real_gl_scissor, "glScissor");
        EnsureRealProc(g_real_gl_viewport, "glViewport");
        if (g_real_gl_bind_framebuffer == nullptr || g_real_gl_clear == nullptr || g_real_gl_clear_color == nullptr
            || g_real_gl_disable == nullptr || g_real_gl_enable == nullptr || g_real_gl_get_floatv == nullptr
            || g_real_gl_get_integerv == nullptr || g_real_gl_is_enabled == nullptr
            || g_real_gl_scissor == nullptr || g_real_gl_viewport == nullptr) {
            return;
        }

        constexpr unsigned int kGlColorBufferBit = 0x00004000;
        constexpr unsigned int kGlColorClearValue = 0x0C22;
        constexpr unsigned int kGlFramebuffer = 0x8D40;
        constexpr unsigned int kGlFramebufferBinding = 0x8CA6;
        constexpr unsigned int kGlScissorBox = 0x0C10;
        constexpr unsigned int kGlScissorTest = 0x0C11;
        constexpr unsigned int kGlViewport = 0x0BA2;
        int previous_framebuffer = 0;
        int previous_viewport[4] = {0, 0, surface_width_, surface_height_};
        int previous_scissor[4] = {0, 0, surface_width_, surface_height_};
        float previous_clear_color[4] = {0, 0, 0, 0};
        g_real_gl_get_integerv(kGlFramebufferBinding, &previous_framebuffer);
        g_real_gl_get_integerv(kGlViewport, previous_viewport);
        g_real_gl_get_integerv(kGlScissorBox, previous_scissor);
        g_real_gl_get_floatv(kGlColorClearValue, previous_clear_color);
        const bool previous_scissor_enabled = g_real_gl_is_enabled(kGlScissorTest) != 0;

        g_real_gl_bind_framebuffer(kGlFramebuffer, 0);
        g_real_gl_viewport(0, 0, surface_width_, surface_height_);
        g_real_gl_enable(kGlScissorTest);
        g_real_gl_clear_color(0, 0, 0, 1);
        const auto clear_rect = [&](const int x, const int y, const int width, const int height) {
            if (width > 0 && height > 0) {
                g_real_gl_scissor(x, y, width, height);
                g_real_gl_clear(kGlColorBufferBit);
            }
        };
        clear_rect(0, 0, letterbox_x_, surface_height_);
        clear_rect(letterbox_x_ + letterbox_width_, 0, surface_width_ - letterbox_x_ - letterbox_width_, surface_height_);
        clear_rect(0, 0, surface_width_, letterbox_y_);
        clear_rect(0, letterbox_y_ + letterbox_height_, surface_width_,
            surface_height_ - letterbox_y_ - letterbox_height_);
        g_real_gl_clear_color(previous_clear_color[0], previous_clear_color[1], previous_clear_color[2], previous_clear_color[3]);
        g_real_gl_scissor(previous_scissor[0], previous_scissor[1], previous_scissor[2], previous_scissor[3]);
        g_real_gl_viewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
        if (previous_scissor_enabled) {
            g_real_gl_enable(kGlScissorTest);
        } else {
            g_real_gl_disable(kGlScissorTest);
        }
        g_real_gl_bind_framebuffer(kGlFramebuffer, static_cast<unsigned int>(previous_framebuffer));
        g_bound_draw_framebuffer = static_cast<unsigned int>(previous_framebuffer);
    }

    bool EnqueueTouch(const HostTouchPhase phase, const int x, const int y, const unsigned long timestamp) {
        const float scale_x = static_cast<float>(letterbox_width_) / static_cast<float>(content_width_);
        const float scale_y = static_cast<float>(letterbox_height_) / static_cast<float>(content_height_);
        const float guest_x = (static_cast<float>(x - letterbox_x_) / scale_x) / 2.0f;
        const float guest_y = (static_cast<float>(y - letterbox_y_) / scale_y) / 2.0f;
        if (phase == HostTouchPhase::Began
            && (x < letterbox_x_ || y < letterbox_y_
                || x >= letterbox_x_ + letterbox_width_ || y >= letterbox_y_ + letterbox_height_)) {
            return false;
        }
        EnqueueHostTouchEvent(HostTouchEvent{
            .pointer_id = 0,
            .phase = phase,
            .x_points = guest_x,
            .y_points = guest_y,
            .timestamp_seconds = static_cast<double>(timestamp) / 1000.0,
        });
        return true;
    }

    void EnqueueKey(XKeyEvent event) {
        KeySym keysym = NoSymbol;
        char buffer[64]{};
        const int length = XLookupString(&event, buffer, sizeof(buffer), &keysym, nullptr);
        if (keysym == XK_BackSpace || keysym == XK_Delete) {
            EnqueueHostKeyEvent(HostKeyEvent{
                .phase = HostKeyPhase::Backspace,
                .timestamp_seconds = static_cast<double>(event.time) / 1000.0,
            });
        } else if (keysym == XK_Return || keysym == XK_KP_Enter) {
            EnqueueHostKeyEvent(HostKeyEvent{
                .phase = HostKeyPhase::Enter,
                .timestamp_seconds = static_cast<double>(event.time) / 1000.0,
            });
        } else if (length > 0) {
            EnqueueHostKeyEvent(HostKeyEvent{
                .phase = HostKeyPhase::Text,
                .text = std::string(buffer, static_cast<std::size_t>(length)),
                .timestamp_seconds = static_cast<double>(event.time) / 1000.0,
            });
        }
    }

    Display* display_ = nullptr;
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    Window window_ = 0;
    Colormap colormap_ = 0;
    Atom wm_delete_window_ = None;
    void* gles_library_ = nullptr;
    int requested_width_ = 0;
    int requested_height_ = 0;
    int content_width_ = 640;
    int content_height_ = 960;
    int surface_width_ = 640;
    int surface_height_ = 960;
    int letterbox_x_ = 0;
    int letterbox_y_ = 0;
    int letterbox_width_ = 640;
    int letterbox_height_ = 960;
    bool mouse_down_ = false;
    bool should_close_ = false;
    bool reported_context_ = false;
};

std::unique_ptr<HostGLBackend> CreateHostGLBackend() {
    return std::make_unique<LinuxHostGLBackend>();
}

#endif
