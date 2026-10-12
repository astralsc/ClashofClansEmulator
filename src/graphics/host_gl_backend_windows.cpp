#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <GL/gl.h>

#include "host_gl_backend.h"

#include "../emulator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

using WglCreateContextAttribsArbFn = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using GlBindFramebufferFn = void(APIENTRY*)(unsigned int, unsigned int);
using GlClearFn = void(APIENTRY*)(unsigned int);
using GlClearColorFn = void(APIENTRY*)(float, float, float, float);
using GlDisableFn = void(APIENTRY*)(unsigned int);
using GlEnableFn = void(APIENTRY*)(unsigned int);
using GlGetFloatvFn = void(APIENTRY*)(unsigned int, float*);
using GlGetIntegervFn = void(APIENTRY*)(unsigned int, int*);
using GlIsEnabledFn = unsigned char(APIENTRY*)(unsigned int);
using GlScissorFn = void(APIENTRY*)(int, int, int, int);
using GlViewportFn = void(APIENTRY*)(int, int, int, int);

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

unsigned int g_bound_draw_framebuffer = 0;
int g_content_width = 960;
int g_content_height = 640;
int g_letterbox_x = 0;
int g_letterbox_y = 0;
int g_letterbox_width = 960;
int g_letterbox_height = 640;

PROC LookupGlProc(const char* name) {
    PROC proc = wglGetProcAddress(name);
    if (proc == nullptr || proc == reinterpret_cast<PROC>(1) || proc == reinterpret_cast<PROC>(2)
        || proc == reinterpret_cast<PROC>(3) || proc == reinterpret_cast<PROC>(-1)) {
        static HMODULE opengl = LoadLibraryW(L"opengl32.dll");
        proc = opengl == nullptr ? nullptr : ::GetProcAddress(opengl, name);
    }
    return proc;
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

int GuestPixelsFromRequest(const int value) {
    return value <= 640 ? std::max(1, value * 2) : std::max(1, value);
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

void WindowsGlBindFramebuffer(const unsigned int target, const unsigned int framebuffer) {
    constexpr unsigned int kGlFramebuffer = 0x8D40;
    constexpr unsigned int kGlDrawFramebuffer = 0x8CA9;
    if (target == kGlFramebuffer || target == kGlDrawFramebuffer) {
        g_bound_draw_framebuffer = framebuffer;
    }
    EnsureRealProc(g_real_gl_bind_framebuffer, "glBindFramebuffer", "glBindFramebufferEXT");
    if (g_real_gl_bind_framebuffer != nullptr) {
        g_real_gl_bind_framebuffer(target, framebuffer);
    }
}

void WindowsGlScissor(const int x, const int y, const int width, const int height) {
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

void WindowsGlViewport(const int x, const int y, const int width, const int height) {
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

constexpr wchar_t kWindowClassName[] = L"SpookyPopEmulatorWindow";

}  // namespace

class WindowsHostGLBackend final : public HostGLBackend {
public:
    ~WindowsHostGLBackend() override {
        if (context_ != nullptr) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(context_);
        }
        if (device_context_ != nullptr && window_ != nullptr) {
            ReleaseDC(window_, device_context_);
        }
        if (window_ != nullptr) {
            DestroyWindow(window_);
        }
    }

    bool IsSupported() const override {
        return true;
    }

    void EnsureWindow(const int width, const int height, const std::string& title) override {
        if (window_ == nullptr) {
            CreateNativeWindow(width, height, title);
            CreateOpenGlContext();
        }
        Resize(width, height);
        MakeCurrent();
        if (!reported_context_) {
            reported_context_ = true;
            std::fprintf(stderr, "[host-gl/windows] WGL window=%dx%d surface=%dx%d context=%p\n",
                width, height, surface_width_, surface_height_, context_);
        }
    }

    void Resize(const int width, const int height) override {
        content_width_ = GuestPixelsFromRequest(width);
        content_height_ = GuestPixelsFromRequest(height);
        if (window_ != nullptr) {
            RECT client{};
            if (GetClientRect(window_, &client) != 0) {
                surface_width_ = std::max(1, static_cast<int>(client.right - client.left));
                surface_height_ = std::max(1, static_cast<int>(client.bottom - client.top));
                UpdateLetterbox();
            }
        }
    }

    void MakeCurrent() override {
        if (device_context_ != nullptr && context_ != nullptr
            && wglMakeCurrent(device_context_, context_) == FALSE) {
            throw std::runtime_error("wglMakeCurrent failed for the Windows window");
        }
    }

    void Present() override {
        if (device_context_ == nullptr) {
            return;
        }
        ClearLetterbox();
        if (SwapBuffers(device_context_) == FALSE) {
            throw std::runtime_error("SwapBuffers failed for the Windows window");
        }
    }

    void PumpEvents() override {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            if (message.message == WM_QUIT) {
                should_close_ = true;
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
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
            EnsureRealProc(g_real_gl_bind_framebuffer, "glBindFramebuffer", "glBindFramebufferEXT");
            return reinterpret_cast<void*>(&WindowsGlBindFramebuffer);
        }
        if (std::strcmp(name, "glScissor") == 0) {
            EnsureRealProc(g_real_gl_scissor, "glScissor");
            return reinterpret_cast<void*>(&WindowsGlScissor);
        }
        if (std::strcmp(name, "glViewport") == 0) {
            EnsureRealProc(g_real_gl_viewport, "glViewport");
            return reinterpret_cast<void*>(&WindowsGlViewport);
        }
        return reinterpret_cast<void*>(LookupGlProc(name));
    }

private:
    static LRESULT CALLBACK WindowProcedure(HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) {
        WindowsHostGLBackend* backend = reinterpret_cast<WindowsHostGLBackend*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            backend = static_cast<WindowsHostGLBackend*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(backend));
        }
        if (backend != nullptr) {
            return backend->HandleWindowMessage(window, message, wparam, lparam);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT HandleWindowMessage(HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam) {
        switch (message) {
        case WM_CLOSE:
            should_close_ = true;
            return 0;
        case WM_DESTROY:
            window_ = nullptr;
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            surface_width_ = std::max(1, static_cast<int>(LOWORD(lparam)));
            surface_height_ = std::max(1, static_cast<int>(HIWORD(lparam)));
            UpdateLetterbox();
            return 0;
        case WM_LBUTTONDOWN:
            mouse_down_ = EnqueueTouch(HostTouchPhase::Began, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (mouse_down_) {
                SetCapture(window);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (mouse_down_) {
                EnqueueTouch(HostTouchPhase::Moved, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            }
            return 0;
        case WM_LBUTTONUP:
            if (mouse_down_) {
                mouse_down_ = false;
                ReleaseCapture();
                EnqueueTouch(HostTouchPhase::Ended, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            }
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_BACK) {
                EnqueueHostKeyEvent(HostKeyEvent{.phase = HostKeyPhase::Backspace, .timestamp_seconds = GetTickCount64() / 1000.0});
            } else if (wparam == VK_RETURN) {
                EnqueueHostKeyEvent(HostKeyEvent{.phase = HostKeyPhase::Enter, .timestamp_seconds = GetTickCount64() / 1000.0});
            }
            return 0;
        case WM_CHAR:
            EnqueueCharacter(static_cast<wchar_t>(wparam));
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }

    void CreateNativeWindow(const int width, const int height, const std::string& title) {
        static const ATOM window_class = [] {
            WNDCLASSEXW definition{};
            definition.cbSize = sizeof(definition);
            definition.style = CS_OWNDC;
            definition.lpfnWndProc = WindowProcedure;
            definition.hInstance = GetModuleHandleW(nullptr);
            definition.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            definition.lpszClassName = kWindowClassName;
            const ATOM registered = RegisterClassExW(&definition);
            if (registered == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                throw std::runtime_error("RegisterClassExW failed for the emulator window");
            }
            return registered;
        }();
        (void)window_class;

        RECT rectangle{0, 0, std::max(1, width), std::max(1, height)};
        if (AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE) == FALSE) {
            throw std::runtime_error("AdjustWindowRect failed for the emulator window");
        }
        const int title_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, title.c_str(), -1, nullptr, 0);
        if (title_length == 0) {
            throw std::runtime_error("Could not convert the emulator window title to UTF-16");
        }
        std::wstring wide_title(static_cast<std::size_t>(title_length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, title.c_str(), -1, wide_title.data(), title_length);

        window_ = CreateWindowExW(0, kWindowClassName, wide_title.c_str(), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
            nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (window_ == nullptr) {
            throw std::runtime_error("CreateWindowExW failed for the emulator window");
        }
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        device_context_ = GetDC(window_);
        if (device_context_ == nullptr) {
            throw std::runtime_error("GetDC failed for the emulator window");
        }
    }

    void CreateOpenGlContext() {
        PIXELFORMATDESCRIPTOR pixel_format{};
        pixel_format.nSize = sizeof(pixel_format);
        pixel_format.nVersion = 1;
        pixel_format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pixel_format.iPixelType = PFD_TYPE_RGBA;
        pixel_format.cColorBits = 32;
        pixel_format.cAlphaBits = 8;
        pixel_format.cDepthBits = 24;
        pixel_format.cStencilBits = 8;
        pixel_format.iLayerType = PFD_MAIN_PLANE;

        const int format = ChoosePixelFormat(device_context_, &pixel_format);
        if (format == 0 || SetPixelFormat(device_context_, format, &pixel_format) == FALSE) {
            throw std::runtime_error("Could not select a double-buffered OpenGL pixel format");
        }
        HGLRC bootstrap_context = wglCreateContext(device_context_);
        if (bootstrap_context == nullptr || wglMakeCurrent(device_context_, bootstrap_context) == FALSE) {
            if (bootstrap_context != nullptr) {
                wglDeleteContext(bootstrap_context);
            }
            throw std::runtime_error("Could not create the initial WGL context");
        }

        const auto create_context = reinterpret_cast<WglCreateContextAttribsArbFn>(
            LookupGlProc("wglCreateContextAttribsARB"));
        if (create_context != nullptr) {
            constexpr int kWglContextMajorVersionArb = 0x2091;
            constexpr int kWglContextMinorVersionArb = 0x2092;
            const int attributes[] = {
                kWglContextMajorVersionArb, 2,
                kWglContextMinorVersionArb, 1,
                0,
            };
            context_ = create_context(device_context_, nullptr, attributes);
        }
        if (context_ != nullptr) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(bootstrap_context);
            if (wglMakeCurrent(device_context_, context_) == FALSE) {
                throw std::runtime_error("Could not activate the WGL 2.1 context");
            }
        } else {
            context_ = bootstrap_context;
        }
        UpdateLetterbox();
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
        EnsureRealProc(g_real_gl_bind_framebuffer, "glBindFramebuffer", "glBindFramebufferEXT");
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
        const bool was_scissor_enabled = g_real_gl_is_enabled(kGlScissorTest) != 0;

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
        clear_rect(letterbox_x_ + letterbox_width_, 0,
            surface_width_ - letterbox_x_ - letterbox_width_, surface_height_);
        clear_rect(0, 0, surface_width_, letterbox_y_);
        clear_rect(0, letterbox_y_ + letterbox_height_, surface_width_,
            surface_height_ - letterbox_y_ - letterbox_height_);

        g_real_gl_clear_color(previous_clear_color[0], previous_clear_color[1], previous_clear_color[2], previous_clear_color[3]);
        g_real_gl_scissor(previous_scissor[0], previous_scissor[1], previous_scissor[2], previous_scissor[3]);
        g_real_gl_viewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
        if (was_scissor_enabled) {
            g_real_gl_enable(kGlScissorTest);
        } else {
            g_real_gl_disable(kGlScissorTest);
        }
        g_real_gl_bind_framebuffer(kGlFramebuffer, static_cast<unsigned int>(previous_framebuffer));
        g_bound_draw_framebuffer = static_cast<unsigned int>(previous_framebuffer);
    }

    bool EnqueueTouch(const HostTouchPhase phase, const int x, const int y) {
        const float scale_x = static_cast<float>(letterbox_width_) / static_cast<float>(content_width_);
        const float scale_y = static_cast<float>(letterbox_height_) / static_cast<float>(content_height_);
        if (phase == HostTouchPhase::Began
            && (x < letterbox_x_ || y < letterbox_y_
                || x >= letterbox_x_ + letterbox_width_ || y >= letterbox_y_ + letterbox_height_)) {
            return false;
        }
        EnqueueHostTouchEvent(HostTouchEvent{
            .pointer_id = 0,
            .phase = phase,
            .x_points = (static_cast<float>(x - letterbox_x_) / scale_x) / 2.0f,
            .y_points = (static_cast<float>(y - letterbox_y_) / scale_y) / 2.0f,
            .timestamp_seconds = static_cast<double>(GetTickCount64()) / 1000.0,
        });
        return true;
    }

    void EnqueueCharacter(const wchar_t character) {
        wchar_t text[2]{character, L'\0'};
        const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, 1, nullptr, 0, nullptr, nullptr);
        if (required <= 0) {
            return;
        }
        std::string utf8(static_cast<std::size_t>(required), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, 1, utf8.data(), required, nullptr, nullptr);
        EnqueueHostKeyEvent(HostKeyEvent{
            .phase = HostKeyPhase::Text,
            .text = std::move(utf8),
            .timestamp_seconds = static_cast<double>(GetTickCount64()) / 1000.0,
        });
    }

    HWND window_ = nullptr;
    HDC device_context_ = nullptr;
    HGLRC context_ = nullptr;
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
    return std::make_unique<WindowsHostGLBackend>();
}

#endif
