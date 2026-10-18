#define CGLTF_IMPLEMENTATION
#include "../vendor/cgltf/cgltf.h"
#include "../src/ecs/GltfImportMath.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
int checks = 0;
int failures = 0;

void check(bool passed, const char* label) {
    ++checks;
    if (passed) std::cout << "[GltfImportTest] PASS: " << label << '\n';
    else {
        std::cerr << "[GltfImportTest] FAIL: " << label << '\n';
        ++failures;
    }
}

bool finite3(const float value[3]) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

void testInterleavedAccessor() {
    const float interleaved[8] = {1.0f, 2.0f, 3.0f, 99.0f, 4.0f, 5.0f, 6.0f, 99.0f};
    cgltf_buffer buffer = {};
    buffer.data = const_cast<float*>(interleaved);
    buffer.size = sizeof(interleaved);
    cgltf_buffer_view view = {};
    view.buffer = &buffer;
    view.size = sizeof(interleaved);
    view.stride = sizeof(float) * 4;
    cgltf_accessor accessor = {};
    accessor.buffer_view = &view;
    accessor.component_type = cgltf_component_type_r_32f;
    accessor.type = cgltf_type_vec3;
    accessor.count = 2;
    accessor.stride = sizeof(float) * 4;
    float value[3] = {};
    const bool read = cgltf_accessor_read_float(&accessor, 1, value, 3) != 0;
    check(read && value[0] == 4.0f && value[1] == 5.0f && value[2] == 6.0f,
          "accessor decoding honors interleaved byte stride");
}

void testNormalizedAccessor() {
    const uint8_t normalized[4] = {0, 128, 255, 64};
    cgltf_buffer buffer = {};
    buffer.data = const_cast<uint8_t*>(normalized);
    buffer.size = sizeof(normalized);
    cgltf_buffer_view view = {};
    view.buffer = &buffer;
    view.size = sizeof(normalized);
    view.stride = 2;
    cgltf_accessor accessor = {};
    accessor.buffer_view = &view;
    accessor.component_type = cgltf_component_type_r_8u;
    accessor.type = cgltf_type_vec2;
    accessor.normalized = 1;
    accessor.count = 2;
    accessor.stride = 2;
    float value[2] = {};
    const bool read = cgltf_accessor_read_float(&accessor, 1, value, 2) != 0;
    check(read && std::abs(value[0] - 1.0f) < 1.0e-6f &&
          std::abs(value[1] - (64.0f / 255.0f)) < 1.0e-6f,
          "accessor decoding normalizes unsigned-byte attributes");
}

void testSparseAccessor() {
    const uint8_t sparseIndex[1] = {1};
    const float sparseValue[3] = {7.0f, 8.0f, 9.0f};
    cgltf_buffer_view indicesView = {};
    indicesView.data = const_cast<uint8_t*>(sparseIndex);
    indicesView.size = sizeof(sparseIndex);
    cgltf_buffer_view valuesView = {};
    valuesView.data = const_cast<float*>(sparseValue);
    valuesView.size = sizeof(sparseValue);
    cgltf_accessor accessor = {};
    accessor.component_type = cgltf_component_type_r_32f;
    accessor.type = cgltf_type_vec3;
    accessor.count = 2;
    accessor.stride = sizeof(sparseValue);
    accessor.is_sparse = 1;
    accessor.sparse.count = 1;
    accessor.sparse.indices_buffer_view = &indicesView;
    accessor.sparse.indices_component_type = cgltf_component_type_r_8u;
    accessor.sparse.values_buffer_view = &valuesView;
    float zeroValue[3] = {1.0f, 1.0f, 1.0f};
    float sparseRead[3] = {};
    const bool zeroRead = cgltf_accessor_read_float(&accessor, 0, zeroValue, 3) != 0;
    const bool sparseReadOk = cgltf_accessor_read_float(&accessor, 1, sparseRead, 3) != 0;
    check(zeroRead && zeroValue[0] == 0.0f && zeroValue[1] == 0.0f && zeroValue[2] == 0.0f &&
          sparseReadOk && sparseRead[0] == 7.0f && sparseRead[1] == 8.0f && sparseRead[2] == 9.0f,
          "accessor decoding applies sparse overrides and zero-initialized base values");
}

bool inspectAsset(const char* path, size_t& primitiveCount, size_t& vertexCount, size_t& triangleCount) {
    cgltf_options options = {};
    cgltf_data* rawData = nullptr;
    if (cgltf_parse_file(&options, path, &rawData) != cgltf_result_success || !rawData) return false;
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(rawData, &cgltf_free);
    if (cgltf_load_buffers(&options, data.get(), path) != cgltf_result_success ||
        cgltf_validate(data.get()) != cgltf_result_success) return false;

    bool valid = true;
    std::unordered_set<const cgltf_node*> visited;
    std::function<void(const cgltf_node*)> visit = [&](const cgltf_node* node) {
        if (!node || !visited.insert(node).second) return;
        float world[16] = {};
        cgltf_node_transform_world(node, world);
        for (float value : world) valid = valid && std::isfinite(value);
        if (node->mesh) {
            for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
                const cgltf_primitive& primitive = node->mesh->primitives[p];
                if (primitive.type != cgltf_primitive_type_triangles) continue;
                const cgltf_accessor* positions = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
                if (!positions || positions->type != cgltf_type_vec3 || positions->count < 3) {
                    valid = false;
                    continue;
                }
                ++primitiveCount;
                vertexCount += positions->count;
                float position[3] = {};
                for (cgltf_size v = 0; v < positions->count; ++v) {
                    valid = valid && cgltf_accessor_read_float(positions, v, position, 3) && finite3(position);
                }
                if (primitive.indices) {
                    if (primitive.indices->count % 3 != 0) valid = false;
                    triangleCount += primitive.indices->count / 3;
                    for (cgltf_size i = 0; i < primitive.indices->count; ++i)
                        valid = valid && cgltf_accessor_read_index(primitive.indices, i) < positions->count;
                } else {
                    if (positions->count % 3 != 0) valid = false;
                    triangleCount += positions->count / 3;
                }
            }
        }
        for (cgltf_size i = 0; i < node->children_count; ++i) visit(node->children[i]);
    };
    if (data->scene) {
        for (cgltf_size i = 0; i < data->scene->nodes_count; ++i) visit(data->scene->nodes[i]);
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
            if (!data->nodes[i].parent) visit(&data->nodes[i]);
    }
    return valid && primitiveCount > 0 && vertexCount > 0 && triangleCount > 0;
}
} // namespace

int main(int argc, char** argv) {
    testInterleavedAccessor();
    testNormalizedAccessor();
    testSparseAccessor();

    size_t primitiveCount = 0, vertexCount = 0, triangleCount = 0;
    std::filesystem::path assetPath;
    if (argc == 2) {
        assetPath = argv[1];
    } else if (argc == 1) {
        const std::filesystem::path relativeAsset = "assets/khronos/MetalRoughSpheresNoTextures.glb";
        const std::filesystem::path sourceAsset =
            std::filesystem::path(__FILE__).parent_path().parent_path() / relativeAsset;
        const std::filesystem::path buildAsset =
            std::filesystem::path("..") / relativeAsset;
        if (std::filesystem::is_regular_file(relativeAsset)) assetPath = relativeAsset;
        else if (std::filesystem::is_regular_file(buildAsset)) assetPath = buildAsset;
        else if (std::filesystem::is_regular_file(sourceAsset)) assetPath = sourceAsset;
    }
    const bool assetValid = !assetPath.empty() && std::filesystem::is_regular_file(assetPath) &&
        inspectAsset(assetPath.string().c_str(), primitiveCount, vertexCount, triangleCount);
    check(assetValid, "bundled Khronos GLB validates and all active-scene geometry accessors decode");
    if (!assetValid) {
        std::cerr << "[GltfImportTest] Could not locate or validate the Khronos GLB";
        if (!assetPath.empty()) std::cerr << ": " << assetPath.string();
        std::cerr << ". Pass its path as the sole argument to override discovery.\n";
    }
    if (assetValid) {
        std::cout << "[GltfImportTest] Active mesh primitives=" << primitiveCount
                  << ", vertices=" << vertexCount << ", triangles=" << triangleCount << '\n';
    }
    if (failures) {
        std::cerr << "[GltfImportTest] " << failures << " / " << checks << " checks failed.\n";
        return 1;
    }
    std::cout << "[GltfImportTest] All " << checks << " checks passed.\n";
    return 0;
}
