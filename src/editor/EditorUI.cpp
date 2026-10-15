#include "EditorUI.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <cstdio>
#include <algorithm>
#include <cctype>

namespace Engine {

void EditorUI::init(D3D12Device& device, bool nativeResolution) {
    m_currentPreset = nativeResolution ? ResolutionPreset::Res_1080p : ResolutionPreset::Scale_75;
    updateRenderResolution();
    m_resolutionChanged = false;
}

void EditorUI::updateRenderResolution() {
    uint32_t width = m_targetRenderWidth;
    uint32_t height = m_targetRenderHeight;
    switch (m_currentPreset) {
    case ResolutionPreset::Res_720p:  width = 1280; height = 720; break;
    case ResolutionPreset::Res_1080p: width = 1920; height = 1080; break;
    case ResolutionPreset::Res_1440p: width = 2560; height = 1440; break;
    case ResolutionPreset::Res_4K:    width = 3840; height = 2160; break;
    case ResolutionPreset::Adaptive_60FPS:
    case ResolutionPreset::Scale_50:
    case ResolutionPreset::Scale_75:
    case ResolutionPreset::Scale_100:
    case ResolutionPreset::Scale_150: {
        float scale = m_currentPreset == ResolutionPreset::Adaptive_60FPS ? m_dynamicResolutionScale : 0.75f;
        if (m_currentPreset == ResolutionPreset::Scale_50) scale = 0.50f;
        else if (m_currentPreset == ResolutionPreset::Scale_100) scale = 1.00f;
        else if (m_currentPreset == ResolutionPreset::Scale_150) scale = 1.50f;
        const auto alignDimension = [](float value) {
            const uint32_t pixels = std::max(64u, static_cast<uint32_t>(value));
            return (pixels + 7u) & ~7u;
        };
        width = alignDimension(m_panelW * scale);
        height = alignDimension(m_panelH * scale);
        break;
    }
    }

    if (width != m_targetRenderWidth || height != m_targetRenderHeight) {
        m_targetRenderWidth = width;
        m_targetRenderHeight = height;
        m_resolutionChanged = true;
    }
}

void EditorUI::updateAndRender(D3D12Device& device, RayTracingPipeline& pipeline, Scene& scene, float deltaTime) {
    // Smooth FPS telemetry
    if (deltaTime > 0.0f) {
        float instantFps = 1.0f / deltaTime;
        m_fps = m_fps * 0.9f + instantFps * 0.1f;
        m_frameTimeMs = deltaTime * 1000.0f;
        m_frameTimeHistory[m_historyIndex] = m_frameTimeMs;
        m_historyIndex = (m_historyIndex + 1) % 60;
    }

    if (m_currentPreset == ResolutionPreset::Adaptive_60FPS && deltaTime > 0.0f) {
        m_dynamicResolutionTimer += deltaTime;
        const float adjustmentInterval = m_fps > 68.0f ? 8.0f : 3.0f;
        if (m_dynamicResolutionTimer >= adjustmentInterval) {
            const float oldScale = m_dynamicResolutionScale;
            if (m_fps < 55.0f) m_dynamicResolutionScale = std::max(0.50f, m_dynamicResolutionScale - 0.05f);
            else if (m_fps > 68.0f) m_dynamicResolutionScale = std::min(1.00f, m_dynamicResolutionScale + 0.025f);
            m_dynamicResolutionTimer = 0.0f;
            if (m_dynamicResolutionScale != oldScale) updateRenderResolution();
        }
    }

    // Main Dockspace
    ImGuiViewport* mainVp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mainVp->Pos);
    ImGui::SetNextWindowSize(mainVp->Size);
    ImGui::SetNextWindowViewport(mainVp->ID);

    ImGuiWindowFlags dockFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking
                               | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse
                               | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                               | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("EngineDockSpace", nullptr, dockFlags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspaceId = ImGui::GetID("EngineDockSpace_ID");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    static bool initLayout = true;
    if (initLayout) {
        initLayout = false;
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, mainVp->Size);

        ImGuiID dockMain = dockspaceId;
        ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.20f, nullptr, &dockMain);
        ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.26f, nullptr, &dockMain);
        ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.25f, nullptr, &dockMain);

        ImGui::DockBuilderDockWindow("Scene Hierarchy", dockLeft);
        ImGui::DockBuilderDockWindow("Inspector", dockRight);
        ImGui::DockBuilderDockWindow("Graphics & GI Settings", dockRight);
        ImGui::DockBuilderDockWindow("Performance Telemetry", dockBottom);
        ImGui::DockBuilderDockWindow("3D Ray Tracing Viewport", dockMain);

        ImGui::DockBuilderFinish(dockspaceId);
    }

    renderMenuBar(scene);
    renderViewport(device, pipeline);

    if (m_showHierarchy) renderSceneHierarchy(scene);
    if (m_showInspector) renderInspector(scene);
    if (m_showSettings)  renderGraphicsSettings(scene, pipeline);
    if (m_showTelemetry) renderTelemetry(device, scene, deltaTime, pipeline.isNrdAvailable());

    ImGui::End();
}

void EditorUI::renderMenuBar(const Scene& scene) {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Scene Hierarchy", nullptr, &m_showHierarchy);
            ImGui::MenuItem("Inspector", nullptr, &m_showInspector);
            ImGui::MenuItem("Graphics & GI Settings", nullptr, &m_showSettings);
            ImGui::MenuItem("Performance Telemetry", nullptr, &m_showTelemetry);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scene")) {
            if (ImGui::MenuItem("Sponza Atrium", nullptr, !scene.isOceanScene() && !scene.isReflectionLabScene())) m_sceneSwitchRequest = 1;
            if (ImGui::MenuItem("Ocean Buoyancy (Jolt)", nullptr, scene.isOceanScene())) m_sceneSwitchRequest = 2;
            if (ImGui::MenuItem("Khronos Metal/Rough Reflection Lab", nullptr, scene.isReflectionLabScene())) m_sceneSwitchRequest = 3;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::Text("Camera Controls:");
            ImGui::BulletText("[W, A, S, D]: Move Camera");
            ImGui::BulletText("[Space / E]: Ascend | [Ctrl / Q]: Descend");
            ImGui::BulletText("[Shift]: Sprint Speed (2.5x)");
            ImGui::BulletText("[Right Mouse Hold]: First-Person Look");
            ImGui::BulletText("[Left Mouse Click]: Ray Pick & Select Object");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void EditorUI::renderViewport(D3D12Device& device, RayTracingPipeline& pipeline) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("3D Ray Tracing Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    ImVec2 vMin = ImGui::GetWindowContentRegionMin();
    ImVec2 vMax = ImGui::GetWindowContentRegionMax();
    ImVec2 winPos = ImGui::GetWindowPos();

    const float panelX = winPos.x + vMin.x;
    const float panelY = winPos.y + vMin.y;
    m_panelW = std::max(vMax.x - vMin.x, 100.0f);
    m_panelH = std::max(vMax.y - vMin.y, 100.0f);
    updateRenderResolution();

    const float renderAspect = static_cast<float>(pipeline.getRenderWidth()) / std::max(1u, pipeline.getRenderHeight());
    m_viewportW = m_panelW;
    m_viewportH = m_panelH;
    if (m_viewportW / m_viewportH > renderAspect) {
        m_viewportW = m_viewportH * renderAspect;
    } else {
        m_viewportH = m_viewportW / renderAspect;
    }
    m_viewportX = panelX + (m_panelW - m_viewportW) * 0.5f;
    m_viewportY = panelY + (m_panelH - m_viewportH) * 0.5f;

    // Draw Ray Traced Output Texture
    auto srvHandle = device.getSrvUavGpuHandle(pipeline.getOutputSrvIndex());
    ImGui::SetCursorScreenPos(ImVec2(m_viewportX, m_viewportY));
    ImGui::Image(static_cast<ImTextureID>(srvHandle.ptr), ImVec2(m_viewportW, m_viewportH));

    // Viewport Top-Left Overlay
    ImGui::SetCursorPos(ImVec2(15.0f, 35.0f));
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.95f, 0.45f, 1.0f));
    ImGui::Text("DXR Hardware Ray Tracing Active");
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.85f, 0.85f));
    ImGui::Text("Resolution: %u x %u | FPS: %.1f (%.2f ms)", pipeline.getRenderWidth(), pipeline.getRenderHeight(), m_fps, m_frameTimeMs);
    ImGui::Text("[RMB]: Look | [WASD]: Move | [LMB]: Select Entity");
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    ImGui::End();
}

void EditorUI::renderSceneHierarchy(Scene& scene) {
    ImGui::Begin("Scene Hierarchy");

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##SearchFilter", "Search names or entity IDs...", m_searchFilter, sizeof(m_searchFilter));

    std::string filterLower(m_searchFilter);
    for (char& c : filterLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto& entities = scene.getEntities();
    ImGui::TextColored(ImVec4(0.55f, 0.65f, 0.85f, 1.0f), "Entities: %u", static_cast<unsigned>(entities.size()));
    std::vector<SceneEntity*> visibleEntities;
    visibleEntities.reserve(entities.size());
    for (auto& ent : entities) {
        if (filterLower.empty()) {
            visibleEntities.push_back(&ent);
            continue;
        }
        std::string nameLower = ent.name.name;
        for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string idText = std::to_string(ent.id);
        if (nameLower.find(filterLower) != std::string::npos || idText.find(filterLower) != std::string::npos) {
            visibleEntities.push_back(&ent);
        }
    }
    EntityID selectedId = scene.getSelectedEntityId();
    ImGui::SameLine();
    if (ImGui::SmallButton("Previous") && !visibleEntities.empty()) {
        auto selected = std::find_if(visibleEntities.begin(), visibleEntities.end(), [selectedId](const SceneEntity* e) { return e->id == selectedId; });
        const size_t index = selected == visibleEntities.end() || selected == visibleEntities.begin()
            ? visibleEntities.size() - 1
            : static_cast<size_t>(std::distance(visibleEntities.begin(), selected) - 1);
        scene.setSelectedEntityId(visibleEntities[index]->id);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Next") && !visibleEntities.empty()) {
        auto selected = std::find_if(visibleEntities.begin(), visibleEntities.end(), [selectedId](const SceneEntity* e) { return e->id == selectedId; });
        const size_t index = selected == visibleEntities.end() ? 0 : (static_cast<size_t>(std::distance(visibleEntities.begin(), selected)) + 1) % visibleEntities.size();
        scene.setSelectedEntityId(visibleEntities[index]->id);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) scene.setSelectedEntityId(NULL_ENTITY);
    ImGui::Separator();

    ImGui::BeginChild("EntityTreeScroll");
    selectedId = scene.getSelectedEntityId();
    uint32_t visibleCount = 0;

    for (const SceneEntity* ent : visibleEntities) {
        ++visibleCount;
        bool isSelected = (selectedId == ent->id);
        const std::string label = ent->name.name + "  |  " + std::to_string(ent->mesh.indexCount / 3) + " tris  |  #" + std::to_string(ent->id);
        ImGui::PushID(static_cast<int>(ent->id));
        if (ImGui::Selectable(label.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
            scene.setSelectedEntityId(ent->id);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Showing %u / %u | Search by name or ID", visibleCount, static_cast<unsigned>(entities.size()));

    ImGui::End();
}

void EditorUI::renderInspector(Scene& scene) {
    ImGui::Begin("Inspector");

    EntityID selectedId = scene.getSelectedEntityId();
    SceneEntity* ent = scene.getEntity(selectedId);

    if (!ent) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No entity selected.");
        ImGui::TextWrapped("Left-click an element in the viewport or Scene Hierarchy to inspect and edit.");
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.55f, 1.0f), "Entity: %s (ID: %u)", ent->name.name.c_str(), ent->id);
    ImGui::Separator();

    // Transform Component
    if (ImGui::CollapsingHeader("Transform Component", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::DragFloat3("Position", &ent->transform.position.x, 0.1f)) scene.markTransformsDirty();
        if (ImGui::DragFloat3("Rotation", &ent->transform.rotation.x, 0.5f)) scene.markTransformsDirty();
        if (ImGui::DragFloat3("Scale", &ent->transform.scale.x, 0.05f, 0.01f, 100.0f)) scene.markTransformsDirty();

        if (ImGui::Button("Reset Transform")) {
            ent->transform.position = Vec3(0, 0, 0);
            ent->transform.rotation = Vec3(0, 0, 0);
            ent->transform.scale    = Vec3(1, 1, 1);
            scene.markTransformsDirty();
        }
    }

    // Material Component
    if (ImGui::CollapsingHeader("Material Component (PBR)", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool matChanged = false;
        if (ImGui::ColorEdit4("Base Color", &ent->material.baseColor.x)) matChanged = true;
        if (ImGui::SliderFloat("Metallic", &ent->material.metallic, 0.0f, 1.0f)) matChanged = true;
        if (ImGui::SliderFloat("Roughness", &ent->material.roughness, 0.04f, 1.0f)) matChanged = true;
        if (ImGui::SliderFloat("Emissive", &ent->material.emissive, 0.0f, 10.0f)) matChanged = true;
        if (matChanged) {
            if (!scene.updateEntityMaterial(ent->id)) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "GPU material update could not be queued.");
            }
            scene.resetAccumulation();
        }
    }

    // Mesh Component Info
    if (ImGui::CollapsingHeader("Mesh Component Info", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Vertices: %u", ent->mesh.vertexCount);
        ImGui::Text("Indices: %u (Triangles: %u)", ent->mesh.indexCount, ent->mesh.indexCount / 3);
        ImGui::Text("BLAS ID: %u", ent->mesh.blasIndex);
    }

    ImGui::End();
}

void EditorUI::renderGraphicsSettings(Scene& scene, RayTracingPipeline& pipeline) {
    ImGui::Begin("Graphics & GI Settings");

    // Resolution Presets
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Render Resolution");
    ImGui::Checkbox("Vertical sync (limits FPS to display refresh)", &m_vsyncEnabled);
    const char* resOptions[] = {
        "1280 x 720 (720p HD)",
        "1920 x 1080 (1080p Full HD)",
        "2560 x 1440 (1440p 2K)",
        "3840 x 2160 (4K UHD)",
        "Adaptive (target 60 FPS)",
        "50% Viewport Scale",
        "75% Viewport Scale",
        "100% Native Viewport",
        "150% Super Sampling"
    };
    ImGui::TextDisabled("Adaptive adjusts render scale between 50%% and 100%% to hold near 60 FPS.");

    int currentResIdx = static_cast<int>(m_currentPreset);
    if (ImGui::Combo("Preset", &currentResIdx, resOptions, IM_ARRAYSIZE(resOptions))) {
        m_currentPreset = static_cast<ResolutionPreset>(currentResIdx);
        updateRenderResolution();
    }

    ImGui::Separator();

    // GI & Lighting
    SceneCB& cb = scene.getSceneCB();
    LightComponent& sun = scene.getSun();

    {
        bool rayTracedReflections = cb.enableRayTracedReflections > 0.5f;
        if (ImGui::Checkbox("DXR specular reflections", &rayTracedReflections)) {
            cb.enableRayTracedReflections = rayTracedReflections ? 1.0f : 0.0f;
            scene.resetAccumulation();
        }
        ImGui::TextDisabled("One roughness-sampled hit for glossy primary surfaces; disable for maximum FPS.");
        ImGui::Separator();
    }

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Path Tracing & Global Illumination");
    bool pathTracingEnabled = cb.renderMode == 0;
    if (ImGui::Checkbox("Enable path tracing and indirect GI", &pathTracingEnabled)) {
        cb.renderMode = pathTracingEnabled ? 0u : 1u;
        scene.resetAccumulation();
    }
    ImGui::TextDisabled("Disabled = direct PBR lighting with DXR shadows; fastest interactive mode.");

    if (pathTracingEnabled && !scene.isOceanScene()) {
        bool nrdEnabled = cb.enableNRD != 0;
        ImGui::BeginDisabled(!pipeline.isNrdAvailable());
        if (ImGui::Checkbox("NVIDIA NRD path denoiser", &nrdEnabled)) {
            cb.enableNRD = nrdEnabled ? 1u : 0u;
            scene.resetAccumulation();
        }
        ImGui::EndDisabled();
        if (pipeline.isNrdAvailable()) {
            ImGui::TextDisabled("REBLUR diffuse: temporal denoising at render resolution.");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f), "NRD unavailable; using the built-in temporal path.");
        }
    }

    const char* debugModes[] = { "Off", "Surface Normals", "Base Albedo" };
    int debugMode = cb.renderMode >= 2 ? static_cast<int>(cb.renderMode - 1) : 0;
    if (ImGui::Combo("Render Debug View", &debugMode, debugModes, IM_ARRAYSIZE(debugModes))) {
        cb.renderMode = debugMode == 0 ? (pathTracingEnabled ? 0u : 1u) : static_cast<uint32_t>(debugMode + 1);
        scene.resetAccumulation();
    }

    if (pathTracingEnabled) {
        int bounces = static_cast<int>(cb.maxBounces);
        if (ImGui::SliderInt("Max Ray Bounces", &bounces, 1, 4)) {
            cb.maxBounces = static_cast<uint32_t>(bounces);
            scene.resetAccumulation();
        }
    }

    ImGui::Separator();

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Sun & Sky Atmosphere");
    bool sunChanged = false;
    bool rayTracedShadows = cb.enableShadows != 0;
    if (ImGui::Checkbox("Ray-traced shadows", &rayTracedShadows)) {
        cb.enableShadows = rayTracedShadows ? 1u : 0u;
        sunChanged = true;
    }
    if (ImGui::DragFloat3("Sun Direction", &sun.direction.x, 0.02f, -1.0f, 1.0f)) {
        sun.direction = sun.direction.normalized();
        sunChanged = true;
    }
    if (ImGui::ColorEdit3("Sun Color", &sun.color.x)) sunChanged = true;
    if (ImGui::SliderFloat("Sun Intensity", &sun.intensity, 0.0f, 20.0f)) sunChanged = true;
    if (rayTracedShadows && ImGui::SliderFloat("Shadow Softness", &cb.shadowSoftness, 0.0f, 1.0f)) sunChanged = true;

    if (ImGui::ColorEdit3("Sky Zenith", &cb.skyZenithColor.x)) sunChanged = true;
    if (ImGui::ColorEdit3("Sky Horizon", &cb.skyHorizonColor.x)) sunChanged = true;

    if (sunChanged) scene.resetAccumulation();

    ImGui::Separator();

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "PostFX & Tone Mapping");
    if (ImGui::SliderFloat("Exposure", &cb.exposure, 0.1f, 5.0f)) scene.resetAccumulation();

    const char* toneModes[] = { "ACES Filmic (Cinematic)", "Reinhard", "Linear" };
    int toneIdx = static_cast<int>(cb.toneMappingMode);
    if (ImGui::Combo("Tone Mapping", &toneIdx, toneModes, IM_ARRAYSIZE(toneModes))) {
        cb.toneMappingMode = static_cast<uint32_t>(toneIdx);
        scene.resetAccumulation();
    }

    ImGui::Separator();

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Temporal Anti-Aliasing & Reprojection");
    bool taaEnabled = (cb.enableTAA != 0);
    if (ImGui::Checkbox("Enable depth-aware temporal AA", &taaEnabled)) {
        cb.enableTAA = taaEnabled ? 1 : 0;
        scene.resetAccumulation();
    }
    if (taaEnabled) {
        ImGui::SliderFloat("TAA Motion Blend Weight", &cb.taaBlendWeight, 0.05f, 0.35f, "%.2f (Lower = smoother)");
    }

    ImGui::End();
}

void EditorUI::renderTelemetry(const D3D12Device& device, const Scene& scene, float deltaTime, bool nrdAvailable) {
    ImGui::Begin("Performance Telemetry");

    ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f), "FPS: %.1f", m_fps);
    ImGui::Text("Frame Time: %.2f ms", m_frameTimeMs);
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Accumulated Samples (SPP): %u", scene.getAccumFrameCount());
    const auto& sceneCb = scene.getSceneCB();
    const bool nrdActive = nrdAvailable && sceneCb.enableNRD != 0 && sceneCb.renderMode == 0;
    const char* aaStatus = nrdActive ? "Active [NVIDIA NRD REBLUR]"
        : (sceneCb.enableTAA != 0 ? "Active [depth-aware TAA]" : "Disabled");
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.6f, 1.0f), "Denoising / AA: %s", aaStatus);

    ImGui::PlotLines("##FrameTimePlot", m_frameTimeHistory, 60, m_historyIndex, "Latency (ms)", 0.0f, 33.3f, ImVec2(-1.0f, 60.0f));

    ImGui::Separator();
    ImGui::Text("GPU: %s", device.getGpuName().c_str());
    ImGui::Text("Raytracing API: DirectX Raytracing (DXR Tier 1.0+)");
    ImGui::Text("Shader Model: HLSL 6.5 (lib_6_5)");

    ImGui::Separator();
    ImGui::Text("Scene Triangles: %u", scene.getTotalTriangles());
    ImGui::Text("Scene Instances: %u", scene.getTotalInstances());

    uint64_t primaryRays = static_cast<uint64_t>(m_targetRenderWidth) * m_targetRenderHeight;
    const uint32_t diffuseBounces = sceneCb.renderMode == 0 ? std::min(sceneCb.maxBounces, 4u) : 0u;
    uint64_t totalPathRays = primaryRays * (1 + diffuseBounces);
    ImGui::Text("Primary Rays/frame: %llu", primaryRays);
    ImGui::Text("Max Primary + Diffuse Rays/frame: %llu", totalPathRays);
    ImGui::TextDisabled("Estimate excludes DXR shadow and ocean reflection rays; misses/early exits lower actual work.");

    ImGui::End();
}

} // namespace Engine
