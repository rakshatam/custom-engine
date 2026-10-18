#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>

#include "core/Window.h"
#include "core/Timer.h"
#include "core/ImageCapture.h"
#include "rhi/D3D12Device.h"
#include "rhi/RayTracingPipeline.h"
#include "ecs/Scene.h"
#include "editor/ImGuiRenderer.h"
#include "editor/EditorUI.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    using namespace Engine;

    const uint32_t windowWidth = 1920;
    const uint32_t windowHeight = 1080;

    std::string fullCmdLine = GetCommandLineA();
    wchar_t executablePath[MAX_PATH] = {};
    const DWORD executablePathLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    const std::wstring executableName = executablePathLength > 0 && executablePathLength < MAX_PATH
        ? std::filesystem::path(executablePath).filename().wstring()
        : std::wstring{};
    const bool launchedAsWaterDemo = !executableName.empty() &&
        CompareStringOrdinal(executableName.c_str(), static_cast<int>(executableName.size()),
                             L"WaterDemo.exe", -1, TRUE) == CSTR_EQUAL;
    const bool waterDemoMode = launchedAsWaterDemo || fullCmdLine.find("--water-demo") != std::string::npos;
    bool oceanTestMode = fullCmdLine.find("--ocean-test") != std::string::npos;
    bool oceanNoBodyDisplacement = fullCmdLine.find("--ocean-no-body-displacement") != std::string::npos;
    bool oceanGif30FpsMode = fullCmdLine.find("--ocean-gif-30fps") != std::string::npos;
    bool oceanSequenceMode = fullCmdLine.find("--ocean-sequence") != std::string::npos || oceanGif30FpsMode;
    bool reflectionCompareMode = fullCmdLine.find("--reflection-compare") != std::string::npos;
    bool khronosReflectionMode = fullCmdLine.find("--reflection-khronos") != std::string::npos;
    bool testMode = oceanTestMode || reflectionCompareMode || (fullCmdLine.find("--test") != std::string::npos) || (fullCmdLine.find("-test") != std::string::npos);
    bool nativeResolutionTest = !oceanTestMode && fullCmdLine.find("--native-resolution") != std::string::npos;

    std::ofstream logFile("engine_run_log.txt");
    logFile << "[Engine] Initializing... TestMode: " << (testMode ? "true" : "false") << std::endl;

    if (testMode) {
        CreateDirectoryA("captures", nullptr);
    }

    Window window;
    const wchar_t* windowTitle = waterDemoMode
        ? L"Custom C++ DX12 Ray Tracing Engine - Water Demo"
        : L"Custom C++ DX12 Ray Tracing Engine - Sponza Atrium";
    if (!window.create(windowTitle, windowWidth, windowHeight)) {
        logFile << "[Engine Error] Failed to create Win32 application window." << std::endl;
        return -1;
    }

    D3D12Device device;
    if (!device.init(window.getHwnd(), window.getWidth(), window.getHeight())) {
        logFile << "[Engine Error] Failed to initialize D3D12 DXR Device." << std::endl;
        return -1;
    }
    logFile << "[Engine] GPU: " << device.getGpuName() << std::endl;
    const char* dxrTier = device.getRaytracingTier() >= D3D12_RAYTRACING_TIER_1_1 ? "1.1" : "1.0";
    logFile << "[DXR] Selected hardware adapter supports Tier " << dxrTier
            << " (ID3D12Device5)." << std::endl;

    EditorUI editorUI;
    editorUI.init(device, nativeResolutionTest);
    logFile << "[Engine] Render resolution: " << editorUI.getTargetRenderWidth() << 'x'
            << editorUI.getTargetRenderHeight() << std::endl;

    RayTracingPipeline pipeline;
    if (!pipeline.init(device, editorUI.getTargetRenderWidth(), editorUI.getTargetRenderHeight())) {
        logFile << "[Engine Error] Failed to initialize Ray Tracing Pipeline." << std::endl;
        return -1;
    }
    logFile << "[NRD] REBLUR diffuse denoiser: " << (pipeline.isNrdAvailable() ? "available" : "unavailable") << std::endl;

    Scene sponzaScene;
    std::string gltfPath = "assets/sponza/Sponza.gltf";
    if (!sponzaScene.init(device, gltfPath)) {
        logFile << "[Engine Error] Failed to initialize Sponza scene." << std::endl;
        return -1;
    }
    logFile << "[Engine] Sponza scene loaded. Triangles: " << sponzaScene.getTotalTriangles() << ", Instances: " << sponzaScene.getTotalInstances() << std::endl;
    Scene oceanScene;
    bool oceanSceneInitialized = false;
    Scene reflectionLabScene;
    bool reflectionLabInitialized = false;
    Scene* activeScene = &sponzaScene;
    if (oceanTestMode || waterDemoMode) {
        if (!oceanScene.initOcean(device)) {
            logFile << "[Engine Error] Ocean scene failed to initialize. Check JoltBridge.dll and MSYS2 UCRT64 runtime staging." << std::endl;
            return -1;
        }
        oceanSceneInitialized = true;
        activeScene = &oceanScene;
        if (oceanTestMode && fullCmdLine.find("--ocean-no-reflections") != std::string::npos) oceanScene.getSceneCB().enableRayTracedReflections = 0.0f;
        if (oceanTestMode && oceanNoBodyDisplacement) {
            logFile << "[Ocean Test] GPU body displacement field disabled for comparison." << std::endl;
        }
        logFile << (oceanTestMode ? "[Ocean Test]" : "[Water Demo]")
                << " Initialized interactive ocean scene with Jolt bodies="
                << oceanScene.getTotalInstances() - 1 << std::endl;
    }
    if (reflectionCompareMode) {
        if (khronosReflectionMode) {
            reflectionLabInitialized = reflectionLabScene.initReflectionLab(device,
                "assets/khronos/MetalRoughSpheresNoTextures.glb");
            if (!reflectionLabInitialized) {
                logFile << "[Reflection Test Error] Khronos Metal/Rough Spheres import failed." << std::endl;
                return -1;
            }
            activeScene = &reflectionLabScene;
            logFile << "[Reflection Lab] Loaded Khronos glTF PBR sample; entities="
                    << reflectionLabScene.getTotalInstances() << ", triangles="
                    << reflectionLabScene.getTotalTriangles() << std::endl;
        }
        SceneCB& reflectionTestCb = activeScene->getSceneCB();
        reflectionTestCb.renderMode = 1;
        reflectionTestCb.maxBounces = 0;
        reflectionTestCb.enableRayTracedReflections = 1.0f;
        reflectionTestCb.enableShadows = 1;
        reflectionTestCb.enableTAA = 0;
        activeScene->setDeterministicSamplingForValidation(true);
        activeScene->resetAccumulation();
        logFile << "[Reflection Test] Scene=" << (khronosReflectionMode ? "Khronos Metal/Rough Spheres" : "Sponza")
                << ", sampling direct PBR + DXR reflections.\n";
    }
    if (testMode && !oceanTestMode && !reflectionCompareMode) sponzaScene.getSceneCB().renderMode = 0; // Sponza GI capture sequence.
    if (!oceanTestMode && fullCmdLine.find("--no-nrd") != std::string::npos) {
        sponzaScene.getSceneCB().enableNRD = 0;
        logFile << "[NRD] Disabled by --no-nrd test override; using built-in accumulation." << std::endl;
    }

    ImGuiRenderer imguiRenderer;
    if (!imguiRenderer.init(device, window.getHwnd())) {
        logFile << "[Engine Error] Failed to initialize ImGui renderer." << std::endl;
        return -1;
    }

    Timer timer;
    int frameNum = 0;
    bool dxrDispatchLogged = false;
    double directModeFrameTimeSum = 0.0;
    uint32_t directModeFrameCount = 0;
    double giFrameTimeSum = 0.0;
    uint32_t giFrameTimeCount = 0;
    double oceanFrameTimeSum = 0.0;
    uint32_t oceanFrameTimeCount = 0;

    logFile << "[Engine] Entering main render loop..." << std::endl;

    while (window.pollEvents()) {
        const float wallDt = timer.tick();
        float dt = oceanTestMode ? (1.0f / 60.0f) : wallDt;

        if (window.isResized()) {
            if (!device.resize(window.getWidth(), window.getHeight())) {
                logFile << "[DXGI Error] Could not restore valid swap-chain targets after resize; shutting down safely.\n";
                return -1;
            }
            window.clearResized();
        }

        const int sceneRequest = editorUI.consumeSceneSwitchRequest();
        if (sceneRequest == 2 && !oceanSceneInitialized) {
            if (oceanScene.initOcean(device)) {
                oceanSceneInitialized = true;
                logFile << "[Scene] Ocean Buoyancy scene activated; Jolt bodies=" << oceanScene.getTotalInstances() - 1 << std::endl;
            } else {
                logFile << "[Scene Error] Ocean scene initialization failed. Sponza remains active." << std::endl;
            }
        }
        if (sceneRequest == 3 && !reflectionLabInitialized) {
            if (reflectionLabScene.initReflectionLab(device, "assets/khronos/MetalRoughSpheresNoTextures.glb")) {
                reflectionLabInitialized = true;
                logFile << "[Scene] Khronos Metal/Rough reflection lab activated.\n";
            } else {
                logFile << "[Scene Error] Khronos reflection lab failed to initialize; current scene remains active.\n";
            }
        }
        Scene* previousScene = activeScene;
        if (sceneRequest == 1) activeScene = &sponzaScene;
        else if (sceneRequest == 2 && oceanSceneInitialized) activeScene = &oceanScene;
        else if (sceneRequest == 3 && reflectionLabInitialized) activeScene = &reflectionLabScene;
        if (activeScene != previousScene) activeScene->resetAccumulation();
        Scene& scene = *activeScene;

        int needCapture = 0;
        int oceanSequenceCaptureFrame = 0;
        if (oceanTestMode) {
            frameNum++;
            if (frameNum >= 30 && frameNum < 360) {
                oceanFrameTimeSum += wallDt;
                ++oceanFrameTimeCount;
            }
            if (frameNum == 360) {
                const double averageMs = oceanFrameTimeCount > 0 ? oceanFrameTimeSum * 1000.0 / oceanFrameTimeCount : 0.0;
                logFile << "[Ocean Test] Frame 360: capture, Jolt bodies=" << scene.getPhysicsBodyCount()
                        << ", simulated=" << scene.getOceanSimulationTime() << " seconds, max body Y displacement="
                        << scene.getOceanMaxVerticalDisplacement() << " m, final body Y range=" << scene.getOceanMinBodyY()
                        << ".." << scene.getOceanMaxBodyY() << " m, impacts=" << scene.getOceanImpactCount()
                        << ", water entries/exits=" << scene.getOceanEntryCrossingCount() << "/"
                        << scene.getOceanExitCrossingCount()
                        << ", average=" << averageMs << " ms ("
                        << (averageMs > 0.0 ? 1000.0 / averageMs : 0.0) << " FPS)" << std::endl;
                needCapture = 8;
            }
            if (oceanSequenceMode) {
                if (oceanGif30FpsMode) {
                    // Physics advances at 60 Hz; capture every second step for
                    // a real 30 fps source sequence instead of stretching sparse stills.
                    if (frameNum >= 30 && frameNum <= 360 && frameNum % 2 == 0) {
                        oceanSequenceCaptureFrame = frameNum;
                    }
                } else {
                    constexpr int captureFrames[] = {30, 45, 60, 75, 90, 105, 120, 150, 180, 240, 300, 360};
                    for (const int captureFrame : captureFrames) {
                        if (frameNum == captureFrame) {
                            oceanSequenceCaptureFrame = captureFrame;
                            break;
                        }
                    }
                }
            }
        } else if (reflectionCompareMode) {
            frameNum++;
            if (frameNum == 65) {
                scene.getSceneCB().enableRayTracedReflections = 0.0f;
                scene.resetAccumulation();
                logFile << "[Reflection Test] Disabled DXR reflections for the off capture.\n";
            } else if (frameNum == 129) {
                scene.getSceneCB().enableRayTracedReflections = 1.0f;
                scene.resetAccumulation();
                logFile << "[Reflection Test] Restored DXR reflections for the final capture.\n";
            }
            if (frameNum >= 10 && frameNum <= 29) {
                directModeFrameTimeSum += dt;
                ++directModeFrameCount;
            } else if (frameNum >= 70 && frameNum <= 127) {
                giFrameTimeSum += dt;
                ++giFrameTimeCount;
            }
            if (frameNum == 64) needCapture = 9;
            else if (frameNum == 128) needCapture = 10;
            else if (frameNum == 192) needCapture = 11;
        } else if (testMode) {
            frameNum++;
            if (frameNum == 20) {
                logFile << "[Test Sequence] Frame 20: Testing Direct Lighting & RT Shadows mode" << std::endl;
                scene.getSceneCB().renderMode = 1;
                scene.resetAccumulation();
            } else if (frameNum == 41) {
                logFile << "[Test Sequence] Frame 41: Testing the RT shadow toggle (off)" << std::endl;
                scene.getSceneCB().enableShadows = 0;
                scene.resetAccumulation();
            } else if (frameNum == 44) {
                logFile << "[Test Sequence] Frame 44: Restoring RT shadows" << std::endl;
                scene.getSceneCB().enableShadows = 1;
                scene.resetAccumulation();
            } else if (frameNum == 45) {
                logFile << "[Test Sequence] Frame 45: Testing Golden Hour sun angle + 3 bounces GI + softer shadows" << std::endl;
                scene.getSceneCB().renderMode = 0;
                scene.getSceneCB().maxBounces = 3;
                scene.getSceneCB().shadowSoftness = 0.75f;
                scene.getSun().direction = Vec3(-0.75f, -0.30f, 0.58f).normalized();
                scene.getSun().color = Vec3(1.0f, 0.70f, 0.35f);
                scene.getSun().intensity = 7.0f;
                scene.resetAccumulation();
            } else if (frameNum == 75) {
                logFile << "[Test Sequence] Frame 75: Testing Reinhard Tone Mapping & high exposure" << std::endl;
                scene.getSceneCB().exposure = 1.6f;
                scene.getSceneCB().toneMappingMode = 1; // Reinhard
                scene.resetAccumulation();
            } else if (frameNum == 95) {
                logFile << "[Test Sequence] Frame 95: Testing Surface Normals Debug Mode" << std::endl;
                scene.getSceneCB().renderMode = 2; // Normals
                scene.resetAccumulation();
            } else if (frameNum == 115) {
                logFile << "[Test Sequence] Frame 115: Restoring Path Traced GI & selecting Sponza entity in ECS Hierarchy" << std::endl;
                scene.getSceneCB().renderMode = 0;
                scene.getSceneCB().toneMappingMode = 0;
                scene.getSceneCB().exposure = 1.25f;
                scene.getSceneCB().maxBounces = 1;
                scene.getSun().direction = Vec3(-0.35f, -0.92f, 0.18f).normalized();
                scene.getSun().color = Vec3(1.0f, 0.96f, 0.88f);
                scene.getSun().intensity = 8.5f;
                scene.resetAccumulation();
                if (!scene.getEntities().empty()) {
                    scene.setSelectedEntityId(scene.getEntities().back().id);
                }
            } else if (frameNum == 232) {
                logFile << "[Test Sequence] Frame 232: exercising per-entity Inspector material upload" << std::endl;
                uint32_t editedEntities = 0;
                for (auto& entity : scene.getEntities()) {
                    entity.material.baseColor = Vec4(1.0f, 0.015f, 0.86f, 1.0f);
                    entity.material.metallic = 0.0f;
                    entity.material.roughness = 0.18f;
                    entity.material.emissive = 8.0f;
                    if (scene.updateEntityMaterial(entity.id)) ++editedEntities;
                }
                scene.resetAccumulation();
                logFile << "[Test Sequence] Queued material overrides for " << editedEntities << " entities." << std::endl;
            }

            if (frameNum >= 125 && frameNum <= 145) {
                // Simulate camera flight motion to exercise temporal reprojection.
                scene.getCamera().position.z += 0.04f;
                scene.getCamera().yaw += 0.08f;
            }

            if (frameNum >= 22 && frameNum <= 39) {
                directModeFrameTimeSum += dt;
                ++directModeFrameCount;
                if (frameNum == 39 && directModeFrameCount > 0) {
                    const double averageMs = directModeFrameTimeSum * 1000.0 / directModeFrameCount;
                    logFile << "[FPS Check] Direct + DXR shadows average: " << averageMs << " ms ("
                            << (1000.0 / averageMs) << " FPS)" << std::endl;
                }
            }

            if (frameNum >= 150 && frameNum <= 169) {
                giFrameTimeSum += dt;
                ++giFrameTimeCount;
                if (frameNum == 169 && giFrameTimeCount > 0) {
                    const double averageMs = giFrameTimeSum * 1000.0 / giFrameTimeCount;
                    logFile << "[FPS Check] GI steady-state average (frames 150-169): " << averageMs << " ms ("
                            << (1000.0 / averageMs) << " FPS)" << std::endl;
                }
            }

            if (frameNum == 18) needCapture = 1;
            else if (frameNum == 40) needCapture = 2;
            else if (frameNum == 70) needCapture = 3;
            else if (frameNum == 90) needCapture = 4;
            else if (frameNum == 110) needCapture = 5;
            else if (frameNum == 135) needCapture = 7;
            else if (frameNum == 230) needCapture = 6; // Capture the ordinary editor before the material regression edit.
            else if (frameNum == 234) needCapture = 8; // Verify edited materials reached the DXR shader.
        }

        if (editorUI.hasResolutionChanged()) {
            if (!pipeline.resizeOutput(device, editorUI.getTargetRenderWidth(), editorUI.getTargetRenderHeight())) {
                logFile << "[DXR Error] Output resize failed; previous render resources and resolution remain active.\n";
            }
            scene.resetAccumulation();
            editorUI.clearResolutionChanged();
        }

        float renderAspect = static_cast<float>(pipeline.getRenderWidth()) / static_cast<float>(pipeline.getRenderHeight());
        scene.update(dt, window, editorUI.getViewportX(), editorUI.getViewportY(), editorUI.getViewportW(),
                     editorUI.getViewportH(), renderAspect, pipeline.getRenderHeight(), device.getFrameIndex());
        if (oceanTestMode && oceanNoBodyDisplacement && scene.isOceanScene()) {
            SceneCB& oceanConstants = scene.getSceneCB();
            const uint32_t bodyCount = std::min(oceanConstants.oceanBodyCount, 8u);
            for (uint32_t bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
                oceanConstants.oceanBodyVerticalState[bodyIndex].z = 0.0f;
            }
        }
        if (oceanSequenceCaptureFrame != 0) {
            logFile << "[Ocean Sequence] Frame " << oceanSequenceCaptureFrame
                    << ": simulated=" << scene.getOceanSimulationTime() << " seconds, impacts="
                    << scene.getOceanImpactCount() << ", crossings=" << scene.getOceanEntryCrossingCount()
                    << "/" << scene.getOceanExitCrossingCount() << ", bodyY=" << scene.getOceanMinBodyY()
                    << ".." << scene.getOceanMaxBodyY() << " m, bodyYs=";
            for (uint32_t bodyIndex = 0; bodyIndex < scene.getPhysicsBodyCount(); ++bodyIndex) {
                if (bodyIndex != 0) logFile << ',';
                logFile << scene.getOceanBodyY(bodyIndex);
            }
            logFile << std::endl;
        }
        if (!pipeline.isNrdAvailable()) scene.getSceneCB().enableNRD = 0;

        device.beginFrame();
        scene.updateGpuBuffers(device);
        const uint32_t uploadedMaterialEdits = scene.consumeUploadedMaterialEdits();
        if (uploadedMaterialEdits > 0) {
            logFile << "[Scene] Uploaded material edits: " << uploadedMaterialEdits << " entity override(s)." << std::endl;
        }

        const DirectX::XMMATRIX viewMatrix = scene.getCamera().getViewMatrix();
        const DirectX::XMMATRIX projectionMatrix = scene.getCamera().getProjMatrix(renderAspect);

        pipeline.dispatch(
            device,
            scene.getTlas().getGpuAddress(),
            scene.getSceneCbGpuAddress(),
            scene.getVertexBufferGpuAddress(),
            scene.getIndexBufferGpuAddress(),
            scene.getMaterialBufferGpuAddress(),
            scene.getInstanceBufferGpuAddress(),
            device.getSrvUavGpuHandle(scene.getTextureArraySrvIndex()),
            pipeline.getRenderWidth(),
            pipeline.getRenderHeight(),
            scene.getSceneCB(),
            projectionMatrix,
            viewMatrix
        );
        if (!dxrDispatchLogged) {
            logFile << "[DXR] DispatchRays recorded for " << pipeline.getRenderWidth() << 'x'
                    << pipeline.getRenderHeight() << " render target." << std::endl;
            dxrDispatchLogged = true;
        }

        if (reflectionCompareMode && needCapture >= 9 && needCapture <= 11) {
            logFile << "[Reflection Test] Capture " << needCapture << " sample=" << scene.getSceneCB().frameCount
                    << " accumulation=" << scene.getSceneCB().accumFrameCount << ".\n";
        }

        if ((needCapture >= 1 && needCapture <= 5) || needCapture == 7 || needCapture == 8 ||
            (needCapture >= 9 && needCapture <= 11) || oceanSequenceCaptureFrame != 0) {
            pipeline.copyOutputToReadback(device);
        }

        imguiRenderer.newFrame();
        editorUI.updateAndRender(device, pipeline, scene, dt);
        imguiRenderer.render(device);

        if (needCapture == 6) {
            imguiRenderer.copyBackBufferToReadback(device, window.getWidth(), window.getHeight());
        }

        device.endFrame(editorUI.isVsyncEnabled());

        if (needCapture != 0 || oceanSequenceCaptureFrame != 0) {
            device.waitForGpu();
            if (oceanSequenceCaptureFrame != 0) {
                std::error_code directoryError;
                const std::string sequenceDirectory = oceanGif30FpsMode
                    ? "captures/ocean_30fps_sequence" : "captures/ocean_sequence";
                std::filesystem::create_directories(sequenceDirectory, directoryError);
                const std::string sequenceFilename = sequenceDirectory + "/frame_"
                    + std::to_string(oceanSequenceCaptureFrame) + ".bmp";
                std::string sequenceError;
                const bool sequenceSaved = !directoryError &&
                    pipeline.saveReadbackToBmp(device, sequenceFilename, &sequenceError);
                logFile << "[Ocean Sequence] Frame " << oceanSequenceCaptureFrame << " saved to "
                        << sequenceFilename << " (" << (sequenceSaved ? "SUCCESS" : "FAILED") << ")";
                if (!sequenceSaved) {
                    logFile << ": " << (directoryError ? directoryError.message() : sequenceError);
                }
                logFile << std::endl;
            }
            if (needCapture != 0) {
            bool ok = false;
            std::string filename;
            std::string captureError;
            if (needCapture == 1) { filename = "captures/01_pathtracing_gi.bmp"; ok = pipeline.saveReadbackToBmp(device, filename, &captureError); }
            else if (needCapture == 2) { filename = "captures/02_direct_shadows.bmp"; ok = pipeline.saveReadbackToBmp(device, filename, &captureError); }
            else if (needCapture == 3) { filename = "captures/03_golden_hour_soft_shadows_gi.bmp"; ok = pipeline.saveReadbackToBmp(device, filename, &captureError); }
            else if (needCapture == 4) { filename = "captures/04_reinhard_exposure.bmp"; ok = pipeline.saveReadbackToBmp(device, filename, &captureError); }
            else if (needCapture == 5) { filename = "captures/05_surface_normals_debug.bmp"; ok = pipeline.saveReadbackToBmp(device, filename, &captureError); }
            else if (needCapture == 6) { filename = "captures/06_full_editor_ui.bmp"; ok = imguiRenderer.saveBackBufferToBmp(filename, window.getWidth(), window.getHeight(), &captureError); }
            else if (needCapture == 7) { filename = "captures/07_camera_motion_taa.bmp"; ok = pipeline.saveReadbackToBmp(device, filename, &captureError); }
            else if (needCapture == 8 && oceanTestMode) { filename = "captures/08_ocean_buoyancy.bmp"; ok = pipeline.saveReadbackToBmp(device, filename, &captureError); }
            else if (needCapture == 8) { filename = "captures/08_material_upload.bmp"; ok = pipeline.saveReadbackToBmp(device, filename, &captureError); }
            else if (needCapture == 9) { filename = "captures/09_reflections_on.bmp"; ok = pipeline.saveReadbackToBmp(device, filename, &captureError); }
            else if (needCapture == 10) { filename = "captures/10_reflections_off.bmp"; ok = pipeline.saveReadbackToBmp(device, filename, &captureError); }
            else if (needCapture == 11) { filename = "captures/11_reflections_restored.bmp"; ok = pipeline.saveReadbackToBmp(device, filename, &captureError); }

            logFile << "[Test Milestone " << needCapture << "] Saved to " << filename << " (" << (ok ? "SUCCESS" : "FAILED") << ")" << std::endl;
            if (!ok) {
                const HRESULT removedReason = device.getDevice() ? device.getDevice()->GetDeviceRemovedReason() : E_FAIL;
                logFile << "[Capture Error] " << (captureError.empty() ? "unknown readback failure" : captureError)
                        << "; device removal reason=0x" << std::hex << static_cast<unsigned long>(removedReason) << std::dec << std::endl;
            }
            if (needCapture == 8 && oceanTestMode && !ok) {
                const HRESULT removedReason = device.getDevice() ? device.getDevice()->GetDeviceRemovedReason() : E_FAIL;
                logFile << "[Ocean Test Error] Capture failed; D3D12 device removal reason=0x" << std::hex
                        << static_cast<unsigned long>(removedReason) << std::dec << std::endl;
            }

            if (needCapture == 6) {
                logFile << "[Engine] Editor UI capture completed; continuing to the Inspector material regression test." << std::endl;
            }
            if (needCapture == 8 && oceanTestMode) {
                logFile << "[Ocean Test] Ocean buoyancy capture " << (ok ? "completed successfully" : "failed") << ". Exiting." << std::endl;
                break;
            }
            if (needCapture == 8 && !oceanTestMode) {
                logFile << "[Engine] Test suite finished " << (ok ? "successfully" : "with a failed material capture") << ". Exiting." << std::endl;
                break;
            }
            if (needCapture == 11 && reflectionCompareMode) {
                const double enabledMs = directModeFrameCount > 0 ? directModeFrameTimeSum * 1000.0 / directModeFrameCount : 0.0;
                const double disabledMs = giFrameTimeCount > 0 ? giFrameTimeSum * 1000.0 / giFrameTimeCount : 0.0;
                logFile << "[Reflection Test] Capture sequence " << (ok ? "completed" : "failed")
                        << "; enabled direct avg=" << enabledMs << " ms (" << (enabledMs > 0.0 ? 1000.0 / enabledMs : 0.0)
                        << " FPS), disabled avg=" << disabledMs << " ms (" << (disabledMs > 0.0 ? 1000.0 / disabledMs : 0.0) << " FPS).\n";
                break;
            }
            }
        }
    }

    device.waitForGpu();
    imguiRenderer.shutdown();
    device.shutdown();

    logFile << "[Engine] Shutdown complete." << std::endl;
    return 0;
}
