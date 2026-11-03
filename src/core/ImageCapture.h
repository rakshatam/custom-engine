#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <system_error>

namespace Engine {

class ImageCapture {
public:
    static bool saveBmp(const std::string& filename, uint32_t width, uint32_t height, const uint8_t* rgbaPixels, uint32_t rowPitch) {
        if (!rgbaPixels || width == 0 || height == 0 ||
            width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
            height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
            std::cerr << "[Capture Error] Refused invalid BMP input: " << filename << ".\n";
            return false;
        }
        const uint64_t sourceRowBytes = static_cast<uint64_t>(width) * 4u;
        const uint64_t bmpRowPitch64 = (static_cast<uint64_t>(width) * 3u + 3u) & ~uint64_t(3u);
        const uint64_t maxBmpImageBytes = std::numeric_limits<uint32_t>::max() - 54u;
        if (sourceRowBytes > rowPitch || bmpRowPitch64 > std::numeric_limits<uint32_t>::max() ||
            bmpRowPitch64 > maxBmpImageBytes / height ||
            static_cast<uint64_t>(rowPitch) > std::numeric_limits<size_t>::max() / height) {
            std::cerr << "[Capture Error] Refused invalid/oversized BMP input: " << filename << ".\n";
            return false;
        }
        const uint64_t imageSize64 = bmpRowPitch64 * height;
        const uint64_t fileSize64 = 54u + imageSize64;

        const std::filesystem::path parent = std::filesystem::path(filename).parent_path();
        if (!parent.empty()) {
            std::error_code directoryError;
            std::filesystem::create_directories(parent, directoryError);
            if (directoryError) {
                std::cerr << "[Capture Error] Could not create BMP output directory '"
                          << parent.string() << "': " << directoryError.message() << ".\n";
                return false;
            }
        }

        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cout << "[Capture] saveBmp failed to open: " << filename << std::endl;
            return false;
        }

        const uint32_t bmpRowPitch = static_cast<uint32_t>(bmpRowPitch64);
        const uint32_t imageSize = static_cast<uint32_t>(imageSize64);
        const uint32_t fileSize = static_cast<uint32_t>(fileSize64);

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
        for (uint32_t rowsRemaining = height; rowsRemaining > 0; --rowsRemaining) {
            const uint32_t y = rowsRemaining - 1;
            const uint8_t* srcRow = rgbaPixels + static_cast<size_t>(y) * rowPitch;
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

        file.flush();
        if (!file) {
            std::cerr << "[Capture Error] Failed while writing BMP: " << filename << ".\n";
            return false;
        }
        file.close();
        if (file.fail()) {
            std::cerr << "[Capture Error] Failed while closing BMP: " << filename << ".\n";
            return false;
        }
        return true;
    }

};

} // namespace Engine
