// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <list>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include "common/thread_worker.h"
#include "video_core/custom_textures/material.h"
#include "video_core/rasterizer_interface.h"

namespace Core {
class System;
}

namespace FileUtil {
struct FSTEntry;
}

namespace VideoCore {

class SurfaceParams;

struct AsyncUpload {
    const Material* material;
    std::function<bool()> func;
};

class CustomTexManager {
public:
    explicit CustomTexManager(Core::System& system);
    ~CustomTexManager();

    /// Sets the application title used for texture load and dump paths.
    void SetTitleId(u64 title_id_) {
        title_id = title_id_;
    }

    /// Processes queued texture uploads
    void TickFrame();

    /// Searches the load directory assigned to program_id for any custom textures and loads them
    void FindCustomTextures();

    /// Reads the pack configuration file
    bool ReadConfig(u64 title_id, bool options_only = false);

    /// Saves the pack configuration file template to the dump directory if it doesn't exist.
    void PrepareDumping(u64 title_id);

    /// Preloads all registered custom textures
    void PreloadTextures(const std::atomic_bool& stop_run,
                         const VideoCore::DiskResourceLoadCallback& callback);

    /// Saves the provided pixel data described by params to disk as png
    void DumpTexture(const SurfaceParams& params, u32 level, std::span<u8> data, u64 data_hash);

    /// Returns the material assigned to the provided data hash
    Material* GetMaterial(u64 data_hash);

    /// Decodes the textures in material to a consumable format and uploads it.
    bool Decode(Material* material, std::function<bool()>&& upload);

    /// True once the decoded custom textures have reached the memory budget.
    bool IsOverMemoryBudget() const noexcept {
        return max_custom_tex_mem != 0 &&
               custom_tex_mem_usage.load(std::memory_order_relaxed) >= max_custom_tex_mem;
    }

    /// Cases the surface cache used to handle wrongly and now steps around; counted and
    /// logged, to see in a log whether one of them comes before a problem.
    enum class Guard : u32 {
        NormalUploadSkipped, ///< The game's pixels were about to go into a replaced surface
        SurfaceRemade,       ///< A replaced surface got a material of another size
        StaleUploadDropped,  ///< A decoded material's surface was gone
        Count,
    };
    void NoteGuard(Guard guard);

    /// Writes the memory in use and the counts above to the log, when they have changed.
    void LogStats();

    /// True when mipmap uploads should be skipped (legacy packs only)
    bool SkipMipmaps() const noexcept {
        return skip_mipmap;
    }

    /// Returns true if the pack uses the new hashing method.
    bool UseNewHash() const noexcept {
        return use_new_hash;
    }

    /// True once a pack with at least one usable texture has been found for this title.
    bool HasMaterials() const noexcept {
        return has_materials;
    }

private:
    /// Parses the custom texture filename (hash, material type, etc).
    bool ParseFilename(const FileUtil::FSTEntry& file, CustomTexture* texture);

    /// Returns a vector of all custom texture files.
    std::vector<FileUtil::FSTEntry> GetTextures(u64 title_id);

    /// Creates the thread workers.
    void CreateWorkers();

    /// Computes the per-session custom-texture memory budget from available memory.
    void ComputeMemoryBudget();

private:
    Frontend::ImageInterface& image_interface;
    std::unordered_set<u64> dumped_textures;
    std::unordered_map<u64, std::unique_ptr<Material>> material_map;
    std::unordered_map<std::string, std::vector<u64>> path_to_hash_map;
    std::vector<std::unique_ptr<CustomTexture>> custom_textures;
    std::list<AsyncUpload> async_uploads;
    std::unique_ptr<Common::ThreadWorker> workers;
    std::atomic<u64> custom_tex_mem_usage{0};
    std::array<u64, static_cast<std::size_t>(Guard::Count)> guard_counts{};
    std::array<u64, static_cast<std::size_t>(Guard::Count)> guard_counts_logged{};
    u64 logged_mem_usage = 0;
    bool logged_over_budget = false;
    std::chrono::steady_clock::time_point last_stats_log{};
    u64 max_custom_tex_mem{0};
    u64 title_id{};
    bool textures_loaded{false};
    bool has_materials{false};
    bool async_custom_loading{true};
    bool skip_mipmap{false};
    bool flip_png_files{true};
    bool use_new_hash{true};
};

} // namespace VideoCore
