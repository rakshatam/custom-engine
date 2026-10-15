#include "Window.h"
#include <windowsx.h>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Engine {

static Window* s_currentWindow = nullptr;

Window::~Window() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool Window::create(const std::wstring& title, uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
    m_hinstance = GetModuleHandle(nullptr);
    s_currentWindow = this;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_CLASSDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = m_hinstance;
    wc.lpszClassName = L"Engine2_RT_WindowClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    RegisterClassExW(&wc);

    RECT wr = { 0, 0, (LONG)m_width, (LONG)m_height };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left,
        wr.bottom - wr.top,
        nullptr, nullptr, m_hinstance, this
    );

    if (!m_hwnd) return false;

    RECT clientRect = {};
    GetClientRect(m_hwnd, &clientRect);
    m_width = clientRect.right - clientRect.left;
    m_height = clientRect.bottom - clientRect.top;

    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);
    m_running = true;
    return true;
}

bool Window::pollEvents() {
    m_mouseDelta = Vec2(0.0f, 0.0f);
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            m_running = false;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return m_running;
}

bool Window::isKeyDown(int vk) const {
    if (vk >= 0 && vk < 256) return m_keys[vk];
    return false;
}

bool Window::isMouseButtonDown(int button) const {
    if (button >= 0 && button < 3) return m_mouseButtons[button];
    return false;
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return true;
    }

    Window* w = s_currentWindow;

    switch (msg) {
    case WM_SIZE:
        if (w && wParam != SIZE_MINIMIZED) {
            w->m_width = LOWORD(lParam);
            w->m_height = HIWORD(lParam);
            w->m_resized = true;
        }
        return 0;

    case WM_KEYDOWN:
        if (w && wParam < 256) w->m_keys[wParam] = true;
        return 0;

    case WM_KEYUP:
        if (w && wParam < 256) w->m_keys[wParam] = false;
        return 0;

    case WM_LBUTTONDOWN:
        if (w) w->m_mouseButtons[0] = true;
        return 0;
    case WM_LBUTTONUP:
        if (w) w->m_mouseButtons[0] = false;
        return 0;

    case WM_RBUTTONDOWN:
        if (w) w->m_mouseButtons[1] = true;
        return 0;
    case WM_RBUTTONUP:
        if (w) w->m_mouseButtons[1] = false;
        return 0;

    case WM_MBUTTONDOWN:
        if (w) w->m_mouseButtons[2] = true;
        return 0;
    case WM_MBUTTONUP:
        if (w) w->m_mouseButtons[2] = false;
        return 0;

    case WM_MOUSEMOVE:
        if (w) {
            float x = static_cast<float>(GET_X_LPARAM(lParam));
            float y = static_cast<float>(GET_Y_LPARAM(lParam));
            w->m_mouseDelta = Vec2(x - w->m_mousePos.x, y - w->m_mousePos.y);
            w->m_mousePos = Vec2(x, y);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        if (w) w->m_running = false;
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace Engine
