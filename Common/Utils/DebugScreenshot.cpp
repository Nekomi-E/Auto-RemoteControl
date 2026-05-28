#include "DebugScreenshot.h"
#include "Logger.h"
#include <windows.h>
#include <cstdio>
#include <ctime>

#pragma pack(push, 2)
struct BmpFileHeader {
    uint16_t bfType = 0x4D42; // "BM"
    uint32_t bfSize = 0;
    uint16_t bfReserved1 = 0;
    uint16_t bfReserved2 = 0;
    uint32_t bfOffBits = 54;
};

struct BmpInfoHeader {
    uint32_t biSize = 40;
    int32_t  biWidth = 0;
    int32_t  biHeight = 0;
    uint16_t biPlanes = 1;
    uint16_t biBitCount = 32;
    uint32_t biCompression = 0;
    uint32_t biSizeImage = 0;
    int32_t  biXPelsPerMeter = 2835;
    int32_t  biYPelsPerMeter = 2835;
    uint32_t biClrUsed = 0;
    uint32_t biClrImportant = 0;
};
#pragma pack(pop)

static std::string MakePath(const std::string& prefix) {
    // Get the executable directory
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    dir = dir.substr(0, dir.rfind('\\') + 1);

    // Timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d_%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return dir + "debug_" + prefix + "_" + ts + ".bmp";
}

std::string SaveBmp(const std::string& prefix, const uint8_t* pixels,
                    uint32_t width, uint32_t height, bool isBgra) {
    if (!pixels || width == 0 || height == 0) return {};

    std::string path = MakePath(prefix);

    BmpFileHeader fileHdr;
    BmpInfoHeader infoHdr;
    infoHdr.biWidth = static_cast<int32_t>(width);
    infoHdr.biHeight = static_cast<int32_t>(-(int32_t)height); // top-down
    uint32_t rowSize = width * 4;
    fileHdr.bfSize = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader) + rowSize * height;

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        LOG_ERROR("SaveBmp: cannot open %s", path.c_str());
        return {};
    }

    fwrite(&fileHdr, sizeof(fileHdr), 1, f);
    fwrite(&infoHdr, sizeof(infoHdr), 1, f);

    if (isBgra) {
        // Data is already BGRA, write row by row
        for (uint32_t y = 0; y < height; ++y) {
            fwrite(pixels + y * rowSize, rowSize, 1, f);
        }
    } else {
        // RGBA → BGRA conversion per pixel
        std::vector<uint8_t> row(rowSize);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t* src = pixels + (y * width + x) * 4;
                uint8_t* dst = row.data() + x * 4;
                dst[0] = src[2]; // B
                dst[1] = src[1]; // G
                dst[2] = src[0]; // R
                dst[3] = src[3]; // A
            }
            fwrite(row.data(), rowSize, 1, f);
        }
    }

    fclose(f);
    LOG_INFO("Saved BMP: %s (%ux%u)", path.c_str(), width, height);
    return path;
}

std::string SaveRawData(const std::string& prefix, const uint8_t* data, size_t len) {
    if (!data || len == 0) return {};

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    dir = dir.substr(0, dir.rfind('\\') + 1);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d_%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::string path = dir + "debug_" + prefix + "_" + ts + ".bin";//使用.bin文件保存原始数据

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        LOG_ERROR("SaveRawData: cannot open %s", path.c_str());
        return {};
    }
    fwrite(data, 1, len, f);
    fclose(f);
    LOG_INFO("Saved raw data: %s (%zu bytes)", path.c_str(), len);
    return path;
}
