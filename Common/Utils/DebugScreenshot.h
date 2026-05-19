#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Save raw BGRA/RGBA pixel data as a BMP file
// Returns the file path on success, empty string on failure
std::string SaveBmp(const std::string& prefix, const uint8_t* pixels,
                    uint32_t width, uint32_t height, bool isBgra = true);

// Save raw H.264 bitstream to file for inspection
std::string SaveRawData(const std::string& prefix, const uint8_t* data, size_t len);
