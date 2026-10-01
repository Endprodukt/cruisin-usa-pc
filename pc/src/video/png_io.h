// Minimal PNG reading / writing (8 bit, non-interlaced) on top of miniz, for texture export and replacement packs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// decodes grey / grey+alpha / RGB / RGBA / palette PNGs (8 bit, not interlaced) to RGBA8
bool png_read_rgba(const std::string &path, int &w, int &h, std::vector<uint8_t> &rgba, std::string *err = nullptr);
bool png_decode_rgba(const uint8_t *data, size_t size, int &w, int &h, std::vector<uint8_t> &rgba, std::string *err = nullptr);
bool png_write_rgba(const std::string &path, int w, int h, const uint8_t *rgba);
