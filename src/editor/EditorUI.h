#pragma once
#include <cstdint>
#include <string>
#include "../rhi/D3D12Device.h"
#include "../rhi/RayTracingPipeline.h"
#include "../ecs/Scene.h"

namespace Engine {

class EditorUI {
public:
    enum class ResolutionPreset {
        Res_720p,
        Res_1080p,
        Res_1440p,
        Res_4K,
        Adaptive_60FPS,
        Scale_50,
        Scale_75,
        Scale_100,
        Scale_150
    };

    EditorUI() = default;
    ~EditorUI() = default;

    void init(D3D12Device& device, bool nativeResolution = false);
    void updateAndRender(D3D12Device& device, RayTracingPipeline& pipeline, Scene& scene, float deltaTime);
    int consumeSceneSwitchRequest() { const int request = m_sceneSwitchRequest; m_sceneSwitchRequest = 0; return request; }

    float getViewportX() const { return m_viewportX; }
    float getViewportY() const { return m_viewportY; }
    float getViewportW() const { return m_viewportW; }
    float getViewportH() const { return m_viewportH; }

    uint32_t getTargetRenderWidth() const { return m_targetRenderWidth; }
    uint32_t getTargetRenderHeight() const { return m_targetRenderHeight; }
    bool hasResolutionChanged() const { return m_resolutionChanged; }
    void clearResolutionChanged() { m_resolutionChanged = false; }
    bool isVsyncEnabled() const { return m_vsyncEnabled; }

private:
    void renderMenuBar(const Scene& scene);
    void renderViewport(D3D12Device& device, RayTracingPipeline& pipeline);
    void renderSceneHierarchy(Scene& scene);
    void renderInspector(Scene& scene);
    void renderGraphicsSettings(Scene& scene, RayTracingPipeline& pipeline);
    void renderTelemetry(const D3D12Device& device, const Scene& scene, float deltaTime, bool nrdAvailable);
    void updateRenderResolution();

    float m_viewportX = 0.0f;
    float m_viewportY = 0.0f;
    float m_viewportW = 1280.0f;
    float m_viewportH = 720.0f;
    float m_panelW = 1280.0f;
    float m_panelH = 720.0f;

    uint32_t m_targetRenderWidth = 960;
    uint32_t m_targetRenderHeight = 540;
    ResolutionPreset m_currentPreset = ResolutionPreset::Scale_75;
    bool m_resolutionChanged = false;
    bool m_vsyncEnabled = false;
    float m_dynamicResolutionScale = 0.75f;
    float m_dynamicResolutionTimer = 0.0f;

    float m_fps = 60.0f;
    float m_frameTimeMs = 16.6f;
    float m_frameTimeHistory[60] = {};
    int   m_historyIndex = 0;

    char m_searchFilter[128] = {};
    bool m_showHierarchy = true;
    bool m_showInspector = true;
    bool m_showSettings = true;
    bool m_showTelemetry = true;
    int m_sceneSwitchRequest = 0;
};

} // namespace Engine
