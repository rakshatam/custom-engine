#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include "Math.h"

namespace Engine {

class Window {
public:
    Window() = default;
    ~Window();

    bool create(const std::wstring& title, uint32_t width, uint32_t height);
    bool pollEvents();

    HWND getHwnd() const { return m_hwnd; }
    HINSTANCE getInstance() const { return m_hinstance; }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    bool isResized() const { return m_resized; }
    void clearResized() { m_resized = false; }
    bool isRunning() const { return m_running; }

    bool isKeyDown(int vk) const;
    bool isMouseButtonDown(int button) const; // 0 = Left, 1 = Right
    Vec2 getMousePos() const { return m_mousePos; }
    Vec2 getMouseDelta() const { return m_mouseDelta; }
    void resetMouseDelta() { m_mouseDelta = Vec2(0.0f, 0.0f); }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hwnd = nullptr;
    HINSTANCE m_hinstance = nullptr;
    uint32_t m_width = 1600;
    uint32_t m_height = 900;
    bool m_running = false;
    bool m_resized = false;

    bool m_keys[256] = {};
    bool m_mouseButtons[3] = {};
    Vec2 m_mousePos = {};
    Vec2 m_prevMousePos = {};
    Vec2 m_mouseDelta = {};
};

} // namespace Engine
