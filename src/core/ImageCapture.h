#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <d3d12.h>
#include "../rhi/D3D12Device.h"

namespace Engine {

class ImageCapture {
public:
    static bool saveBmp(const std::string& filename, uint32_t width, uint32_t height, const uint8_t* rgbaPixels, uint32_t rowPitch) {
        // Ensure parent directory exists
        size_t slash = filename.find_last_of("/\\");
        if (slash != std::string::npos) {
            std::string dir = filename.substr(0, slash);
            CreateDirectoryA(dir.c_str(), nullptr);
        }

        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cout << "[Capture] saveBmp failed to open: " << filename << std::endl;
            return false;
        }

        uint32_t bmpRowPitch = (width * 3 + 3) & ~3;
        uint32_t imageSize = bmpRowPitch * height;
        uint32_t fileSize = 54 + imageSize;

        // 14-byte File Header
        uint8_t fileHeader[14] = {
            'B', 'M',
            static_cast<uint8_t>(fileSize & 0xFF),
            static_cast<uint8_t>((fileSize >> 8) & 0xFF),
            static_cast<uint8_t>((fileSize >> 16) & 0xFF),
            static_cast<uint8_t>((fileSize >> 24) & 0xFF),
            0, 0, 0, 0, // reserved
            54, 0, 0, 0  // offset to pixel data
        };

        // 40-byte Info Header
        uint8_t infoHeader[40] = {
            40, 0, 0, 0, // biSize
            static_cast<uint8_t>(width & 0xFF),
            static_cast<uint8_t>((width >> 8) & 0xFF),
            static_cast<uint8_t>((width >> 16) & 0xFF),
            static_cast<uint8_t>((width >> 24) & 0xFF),
            static_cast<uint8_t>(height & 0xFF),
            static_cast<uint8_t>((height >> 8) & 0xFF),
            static_cast<uint8_t>((height >> 16) & 0xFF),
            static_cast<uint8_t>((height >> 24) & 0xFF),
            1, 0,        // planes
            24, 0,       // 24-bit RGB
            0, 0, 0, 0,  // uncompressed
            static_cast<uint8_t>(imageSize & 0xFF),
            static_cast<uint8_t>((imageSize >> 8) & 0xFF),
            static_cast<uint8_t>((imageSize >> 16) & 0xFF),
            static_cast<uint8_t>((imageSize >> 24) & 0xFF),
            0x13, 0x0B, 0, 0, // 2835 ppm
            0x13, 0x0B, 0, 0,
            0, 0, 0, 0,
            0, 0, 0, 0
        };

        file.write(reinterpret_cast<const char*>(fileHeader), 14);
        file.write(reinterpret_cast<const char*>(infoHeader), 40);

        std::vector<uint8_t> rowBuffer(bmpRowPitch, 0);

        // BMP rows are stored bottom-up
        for (int y = static_cast<int>(height) - 1; y >= 0; --y) {
            const uint8_t* srcRow = rgbaPixels + y * rowPitch;
            for (uint32_t x = 0; x < width; ++x) {
                uint8_t r = srcRow[x * 4 + 0];
                uint8_t g = srcRow[x * 4 + 1];
                uint8_t b = srcRow[x * 4 + 2];
                rowBuffer[x * 3 + 0] = b; // BGR order
                rowBuffer[x * 3 + 1] = g;
                rowBuffer[x * 3 + 2] = r;
            }
            file.write(reinterpret_cast<const char*>(rowBuffer.data()), bmpRowPitch);
        }

        return true;
    }

    struct CaptureContext {
        Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowPitch = 0;
        std::string filename;
        bool valid = false;
    };

    static CaptureContext prepareCapture(D3D12Device& device, ID3D12Resource* sourceTexture, uint32_t width, uint32_t height, D3D12_RESOURCE_STATES currentState, const std::string& filename) {
        CaptureContext ctx;
        ctx.width = width;
        ctx.height = height;
        ctx.filename = filename;

        D3D12_RESOURCE_DESC texDesc = sourceTexture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
        UINT64 totalBytes = 0;
        device.getDevice()->GetCopyableFootprints(&texDesc, 0, 1, 0, &layout, nullptr, nullptr, &totalBytes);

        ctx.rowPitch = layout.Footprint.RowPitch;
        ctx.readbackBuffer = device.createBuffer(totalBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

        if (!ctx.readbackBuffer) {
            char errBuf[256];
            snprintf(errBuf, sizeof(errBuf), "[Capture Error] Failed to create readbackBuffer size=%llu rowPitch=%u for %s\n",
                     totalBytes, ctx.rowPitch, filename.c_str());
            OutputDebugStringA(errBuf);
            std::cout << errBuf;
            ctx.valid = false;
            return ctx;
        }

        auto cmdList = device.getCommandList();
        device.transitionBarrier(sourceTexture, currentState, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = ctx.readbackBuffer.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = layout;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = sourceTexture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        device.transitionBarrier(sourceTexture, D3D12_RESOURCE_STATE_COPY_SOURCE, currentState);

        ctx.valid = true;
        return ctx;
    }

    static bool finishCapture(CaptureContext& ctx, std::string& err) {
        if (!ctx.valid) {
            err = "ctx not valid";
            return false;
        }
        if (!ctx.readbackBuffer) {
            err = "readbackBuffer is null";
            return false;
        }
        uint8_t* mappedData = nullptr;
        HRESULT hr = ctx.readbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedData));
        if (FAILED(hr)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Map failed hr=0x%08X", static_cast<unsigned int>(hr));
            err = buf;
            return false;
        }
        if (!mappedData) {
            err = "mappedData is null pointer";
            ctx.readbackBuffer->Unmap(0, nullptr);
            return false;
        }
        bool ok = saveBmp(ctx.filename, ctx.width, ctx.height, mappedData, ctx.rowPitch);
        if (!ok) {
            err = "saveBmp failed to write file: " + ctx.filename;
        }
        ctx.readbackBuffer->Unmap(0, nullptr);
        ctx.valid = false;
        return ok;
    }

    static bool finishCapture(CaptureContext& ctx) {
        std::string dummy;
        return finishCapture(ctx, dummy);
    }
};

} // namespace Engine
