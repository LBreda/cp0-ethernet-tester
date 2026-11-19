/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file asset_manager.h
 * @brief Locates application asset directories and loads LVGL fonts from them.
 */

#pragma once

#include "lvgl.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace app {

/** @brief Weight variants of the bundled standard font. */
enum class StandardFontWeight {
    Regular,
    Bold,
};

/**
 * @brief Resolves asset paths across a list of candidate roots and owns loaded fonts.
 *
 * Fonts returned by the load functions stay valid for the lifetime of the manager.
 */
class AssetManager {
public:
    explicit AssetManager(std::string app_name = default_app_name(),
                          std::filesystem::path cmake_assets_root = default_cmake_assets_root());
    ~AssetManager();

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    /** @brief Application name used to build installed asset paths. */
    static std::string default_app_name();
    /** @brief Asset directory of the source tree, configured at build time. */
    static std::filesystem::path default_cmake_assets_root();

    /** @brief Existing asset roots, in search order. */
    const std::vector<std::filesystem::path>& roots() const;
    /** @brief Returns the first existing match of @p relative_path under the roots. */
    std::filesystem::path resolve(const std::filesystem::path& relative_path) const;
    /** @brief Resolves a font file name inside the fonts asset directory. */
    std::filesystem::path resolve_font(const std::filesystem::path& file_name) const;

    /**
     * @brief Loads (or reuses) the font @p file_name at pixel @p size.
     * @return The font, or nullptr if it could not be loaded.
     */
    lv_font_t* load_font(const std::filesystem::path& file_name, uint32_t size);
    /** @brief Loads the bundled standard font with the given size and weight. */
    lv_font_t* load_standard_font(uint32_t size,
                                  StandardFontWeight weight = StandardFontWeight::Bold);

private:
    struct LoadedFont;

    void add_root_if_valid(std::filesystem::path path);

    std::vector<std::filesystem::path> roots_;
    std::vector<std::unique_ptr<LoadedFont>> loaded_fonts_;
};

} // namespace app
