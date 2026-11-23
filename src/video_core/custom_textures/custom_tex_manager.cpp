// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <json.hpp>
#include "common/file_util.h"
#include "common/horizon_meminfo.h"
#include "common/horizon_thread.h"
#include "common/literals.h"
#include "common/memory_detect.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "common/thread.h"
#include "common/texture.h"
#include "core/core.h"
#include "core/frontend/image_interface.h"
#include "video_core/custom_textures/custom_tex_manager.h"
#include "video_core/rasterizer_cache/surface_params.h"
#include "video_core/rasterizer_cache/utils.h"

namespace VideoCore {

namespace {

MICROPROFILE_DEFINE(CustomTexManager_TickFrame, "CustomTexManager", "TickFrame",
                    MP_RGB(54, 16, 32));

constexpr std::size_t MAX_UPLOADS_PER_TICK = 8;

/// How much picture data one frame copies to the GPU. Eight pictures of a 4K pack are
/// well over 100 MiB, copied by the thread that draws the frame: a visible stop when an
/// area is entered. Spread over more frames the pictures appear one after another
/// instead. One upload always goes through, whatever its size.
constexpr u64 MAX_UPLOAD_BYTES_PER_TICK = 24ULL * 1024 * 1024;

// A resident custom texture costs roughly its decoded size twice.
constexpr u64 CUSTOM_TEX_MEM_FACTOR = 2;

using namespace Common::Literals;

bool IsPow2(u32 value) {
    return value != 0 && (value & (value - 1)) == 0;
}

CustomFileFormat MakeFileFormat(std::string_view ext) {
    if (ext == "png") {
        return CustomFileFormat::PNG;
    } else if (ext == "dds") {
        return CustomFileFormat::DDS;
    } else if (ext == "ktx") {
        return CustomFileFormat::KTX;
    }
    return CustomFileFormat::None;
}

MapType MakeMapType(std::string_view ext) {
    if (ext == "norm") {
        return MapType::Normal;
    }
    LOG_ERROR(Render, "Unknown material extension {}", ext);
    return MapType::Color;
}

} // Anonymous namespace

CustomTexManager::CustomTexManager(Core::System& system_)
    : image_interface{*system_.GetImageInterface()},
      async_custom_loading{Settings::values.async_custom_loading.GetValue()} {}

CustomTexManager::~CustomTexManager() = default;

void CustomTexManager::TickFrame() {
    MICROPROFILE_SCOPE(CustomTexManager_TickFrame);
    if (!textures_loaded) {
        return;
    }
    EvictDecoded();
    LogStats();
    std::size_t num_uploads = 0;
    u64 uploaded_bytes = 0;
    for (auto it = async_uploads.begin(); it != async_uploads.end();) {
        if (num_uploads >= MAX_UPLOADS_PER_TICK) {
            return;
        }
        switch (it->material->state) {
        case DecodeState::Decoded:
            if (num_uploads > 0 &&
                uploaded_bytes + it->material->size > MAX_UPLOAD_BYTES_PER_TICK) {
                return;
            }
            uploaded_bytes += it->material->size;
            it->func();
            num_uploads++;
            [[fallthrough]];
        case DecodeState::Failed:
            it = async_uploads.erase(it);
            continue;
        default:
            it++;
            break;
        }
    }
}

void CustomTexManager::NoteGuard(Guard guard) {
    static constexpr std::array<const char*, static_cast<std::size_t>(Guard::Count)> NAMES = {
        "the game's pixels kept out of a replaced surface",
        "a replaced surface remade for a material of another size",
        "a decoded material dropped, its surface was gone",
    };
    const std::size_t index = static_cast<std::size_t>(guard);
    // The first of each kind at once, with the time it happened; the rest in LogStats.
    if (guard_counts[index]++ == 0) {
        LOG_WARNING(Render, "Custom textures: {} (first time)", NAMES[index]);
    }
}

void CustomTexManager::LogStats() {
    if (!has_materials) {
        return;
    }
    const u64 usage = custom_tex_mem_usage.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    const bool over = IsOverMemoryBudget();
    if (over && (!logged_over_budget || now - last_budget_log > std::chrono::seconds{60})) {
        last_budget_log = now;
        LOG_WARNING(Render,
                    "Custom textures: memory is short ({} MiB left of the heap, {} MiB of pictures "
                    "decoded); textures not loaded yet stay as the game's own until there is room",
                    heap_free / 1_MiB, usage / CUSTOM_TEX_MEM_FACTOR / 1_MiB);
    }
    logged_over_budget = over;
    if (now - last_stats_log < std::chrono::seconds{60}) {
        return;
    }
    last_stats_log = now;
    if (usage == logged_mem_usage && guard_counts == guard_counts_logged &&
        evicted_count == evicted_count_logged) {
        return;
    }
    logged_mem_usage = usage;
    guard_counts_logged = guard_counts;
    evicted_count_logged = evicted_count;
    LOG_INFO(Render,
             "Custom textures: {} MiB of pictures decoded, {} MiB left of the heap (least seen "
             "{} MiB), {} uploads waiting, {} pictures freed after use ({} MiB) | kept the game's "
             "pixels out of a replaced surface {}x, remade a replaced surface {}x, dropped a "
             "decoded material {}x",
             usage / CUSTOM_TEX_MEM_FACTOR / 1_MiB, heap_free / 1_MiB, heap_free_min / 1_MiB,
             async_uploads.size(), evicted_count, evicted_bytes / 1_MiB, guard_counts[0],
             guard_counts[1], guard_counts[2]);
}

void CustomTexManager::EvictDecoded() {
    // A decoded picture is only needed until it has been uploaded: the GPU has its own copy
    // from then on. They used to be kept for the whole session, 64 MiB for each 4096x4096
    // one, until a long session with a 4K pack had no memory left: a picture failed to
    // decode and the GPU was lost. They are kept for a while, since the game often asks for
    // the same one again (item icons), and the ones not asked for longest go first.
    const auto now = std::chrono::steady_clock::now();
    if (now - last_evict_check < std::chrono::milliseconds{500}) {
        return;
    }
    last_evict_check = now;
    heap_free = Common::GetHorizonHeapFree();
    if (heap_free == 0) {
        // Not a console: nothing is known about the memory left, keep them as before.
        return;
    }
    if (heap_free_min == 0 || heap_free < heap_free_min) {
        heap_free_min = heap_free;
    }
    static constexpr u64 KEEP = 1024_MiB;
    static constexpr u64 KEEP_WHEN_SHORT = 128_MiB;
    static constexpr u64 SHORT = 1536_MiB;
    const u64 keep = (heap_free < SHORT ? KEEP_WHEN_SHORT : KEEP) * CUSTOM_TEX_MEM_FACTOR;
    const u64 usage = custom_tex_mem_usage.load(std::memory_order_relaxed);
    if (usage <= keep) {
        return;
    }

    // Pictures a worker may be decoding now, or an upload still to come will read.
    std::unordered_set<const CustomTexture*> busy;
    const auto mark_busy = [&busy](const Material& material) {
        for (const CustomTexture* texture : material.textures) {
            if (texture) {
                busy.insert(texture);
            }
        }
    };
    for (const AsyncUpload& upload : async_uploads) {
        mark_busy(*upload.material);
    }
    std::vector<Material*> candidates;
    for (const auto& [hash, material] : material_map) {
        if (material->IsPending()) {
            mark_busy(*material);
        } else if (material->IsDecoded()) {
            candidates.push_back(material.get());
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Material* a, const Material* b) { return a->last_used < b->last_used; });

    u64 freed = 0;
    std::unordered_set<const CustomTexture*> gone;
    for (Material* const material : candidates) {
        if (usage - freed <= keep) {
            break;
        }
        const bool in_use = std::any_of(
            material->textures.begin(), material->textures.end(),
            [&busy](const CustomTexture* texture) { return texture && busy.contains(texture); });
        if (in_use) {
            continue;
        }
        for (CustomTexture* const texture : material->textures) {
            if (texture && texture->IsLoaded()) {
                freed += texture->data.size() * CUSTOM_TEX_MEM_FACTOR;
                std::vector<u8>{}.swap(texture->data);
                gone.insert(texture);
                ++evicted_count;
            }
        }
    }
    if (gone.empty()) {
        return;
    }
    // A picture can belong to several materials: all of them have to be loaded again.
    for (const auto& [hash, material] : material_map) {
        if (!material->IsDecoded()) {
            continue;
        }
        const bool lost = std::any_of(
            material->textures.begin(), material->textures.end(),
            [&gone](const CustomTexture* texture) { return texture && gone.contains(texture); });
        if (lost) {
            material->size = 0;
            material->state = DecodeState::None;
        }
    }
    evicted_bytes += freed / CUSTOM_TEX_MEM_FACTOR;
    custom_tex_mem_usage.fetch_sub(std::min(freed, usage), std::memory_order_relaxed);
}

void CustomTexManager::FindCustomTextures() {
    if (textures_loaded) {
        return;
    }
    if (!workers) {
        CreateWorkers();
    }
    ComputeMemoryBudget();

    const auto textures = GetTextures(title_id);
    if (!ReadConfig(title_id)) {
        use_new_hash = false;
        skip_mipmap = true;
    }

    custom_textures.reserve(textures.size());
    for (const FileUtil::FSTEntry& file : textures) {
        if (file.isDirectory) {
            continue;
        }
        custom_textures.push_back(std::make_unique<CustomTexture>(image_interface));
        CustomTexture* const texture{custom_textures.back().get()};
        if (!ParseFilename(file, texture)) {
            continue;
        }
        for (const u64 hash : texture->hashes) {
            auto& material = material_map[hash];
            if (!material) {
                material = std::make_unique<Material>();
            }
            material->hash = hash;
            material->AddMapTexture(texture);
        }
    }
    has_materials = !material_map.empty();
    textures_loaded = true;
    if (!has_materials) {
        LOG_INFO(Render, "No custom textures found for title {:016X}",
                 title_id);
    }
}

bool CustomTexManager::ParseFilename(const FileUtil::FSTEntry& file, CustomTexture* texture) {
    auto parts = Common::SplitString(file.virtualName, '.');
    if (parts.size() > 3) {
        LOG_ERROR(Render, "Invalid filename {}, ignoring", file.virtualName);
        return false;
    }
    // The last string should always be the file extension.
    const CustomFileFormat file_format = MakeFileFormat(parts.back());
    if (file_format == CustomFileFormat::None) {
        return false;
    }
    if (file_format == CustomFileFormat::DDS && skip_mipmap) {
        LOG_ERROR(Render, "Mipmap skip is incompatible with DDS textures, skipping!");
        return false;
    }
    texture->file_format = file_format;
    parts.pop_back();

    // This means the texture is a material type other than color.
    texture->type = MapType::Color;
    if (parts.size() > 1) {
        texture->type = MakeMapType(parts.back());
        parts.pop_back();
    }

    // First look if this file is mapped to any number of hashes.
    std::vector<u64>& hashes = texture->hashes;
    const auto it = path_to_hash_map.find(file.virtualName);
    if (it != path_to_hash_map.end()) {
        hashes = it->second;
    }

    // It's also possible for pack creators to retain the default texture name
    // still map the texture to another hash. Support that as well.
    u32 width;
    u32 height;
    u32 format;
    unsigned long long hash{};
    const bool is_parsed = std::sscanf(parts.back().c_str(), "tex1_%ux%u_%llX_%u", &width, &height,
                                       &hash, &format) == 4;
    const bool is_mapped =
        !hashes.empty() && std::find(hashes.begin(), hashes.end(), hash) != hashes.end();
    if (is_parsed && !is_mapped) {
        hashes.push_back(hash);
    }

    texture->path = file.physicalName;
    return true;
}

void CustomTexManager::PrepareDumping(u64 title_id) {
    // If a pack exists in the load folder that uses the old hash, dump textures using the old hash.
    // This occurs either if a configuration file doesn't exist or that file sets the old hash.
    const std::string load_path =
        fmt::format("{}textures/{:016X}/", GetUserPath(FileUtil::UserPath::LoadDir), title_id);
    if (FileUtil::Exists(load_path) && !ReadConfig(title_id, true)) {
        use_new_hash = false;
    }

    // Write template config file
    const std::string dump_path =
        fmt::format("{}textures/{:016X}/", GetUserPath(FileUtil::UserPath::DumpDir), title_id);
    const std::string pack_config = dump_path + "pack.json";
    if (FileUtil::Exists(pack_config)) {
        return;
    }

    nlohmann::ordered_json json;
    json["author"] = "citra";
    json["version"] = "1.0.0";
    json["description"] = "A graphics pack";

    auto& options = json["options"];
    options["skip_mipmap"] = false;
    options["flip_png_files"] = true;
    options["use_new_hash"] = true;

    FileUtil::IOFile file{pack_config, "w"};
    const std::string output = json.dump(4);
    file.WriteString(output);
}

void CustomTexManager::PreloadTextures(const std::atomic_bool& stop_run,
                                       const VideoCore::DiskResourceLoadCallback& callback) {
    u64 size_sum = 0;
    std::size_t preloaded = 0;
    const u64 sys_mem = Common::GetMemInfo().total_physical_memory;
    const u64 recommended_min_mem = 2_GiB;

    // keep 2GiB memory for system stability if system RAM is 4GiB+ - use half of memory in other
    // cases
    const u64 max_mem =
        (sys_mem / 2 < recommended_min_mem) ? (sys_mem / 2) : (sys_mem - recommended_min_mem);

    workers->QueueWork([&]() {
        for (auto& [hash, material] : material_map) {
            if (size_sum > max_mem) {
                LOG_WARNING(Render, "Aborting texture preload due to insufficient memory");
                return;
            }
            if (stop_run) {
                return;
            }
            material->LoadFromDisk(flip_png_files);
            size_sum += material->size;
            if (callback) {
                callback(VideoCore::LoadCallbackStage::Preload, preloaded, custom_textures.size(),
                         "");
            }
            preloaded++;
        }
    });
    workers->WaitForRequests();
    async_custom_loading = false;
}

void CustomTexManager::DumpTexture(const SurfaceParams& params, u32 level, std::span<u8> data,
                                   u64 data_hash) {
    const u32 data_size = static_cast<u32>(data.size());
    const u32 width = params.width;
    const u32 height = params.height;
    const PixelFormat format = params.pixel_format;

    std::string dump_path = fmt::format(
        "{}textures/{:016X}/", FileUtil::GetUserPath(FileUtil::UserPath::DumpDir), title_id);
    if (!FileUtil::CreateFullPath(dump_path)) {
        LOG_ERROR(Render, "Unable to create {}", dump_path);
        return;
    }

    dump_path +=
        fmt::format("tex1_{}x{}_{:016X}_{}_mip{}.png", width, height, data_hash, format, level);
    if (dumped_textures.contains(data_hash) || FileUtil::Exists(dump_path)) {
        return;
    }

    // Make sure the texture size is a power of 2.
    // If not, the surface is probably a framebuffer
    if (!IsPow2(width) || !IsPow2(height)) {
        LOG_WARNING(Render, "Not dumping {:016X} because size isn't a power of 2 ({}x{})",
                    data_hash, width, height);
        return;
    }

    const u32 decoded_size = width * height * 4;
    std::vector<u8> pixels(data_size + decoded_size);
    std::memcpy(pixels.data(), data.data(), data_size);

    auto dump = [this, width, height, params, data_size, decoded_size, pixels = std::move(pixels),
                 dump_path = std::move(dump_path)]() mutable {
        const std::span encoded = std::span{pixels}.first(data_size);
        const std::span decoded = std::span{pixels}.last(decoded_size);
        DecodeTexture(params, params.addr, params.end, encoded, decoded,
                      params.type == SurfaceType::Color);
        Common::FlipRGBA8Texture(decoded, width, height);
        image_interface.EncodePNG(dump_path, width, height, decoded);
    };
    if (!workers) {
        CreateWorkers();
    }
    workers->QueueWork(std::move(dump));
    dumped_textures.insert(data_hash);
}

Material* CustomTexManager::GetMaterial(u64 data_hash) {
    const auto it = material_map.find(data_hash);
    if (it == material_map.end()) {
        LOG_DEBUG(Render, "Unable to find replacement for surface with hash {:016X}", data_hash);
        return nullptr;
    }
    return it->second.get();
}

bool CustomTexManager::Decode(Material* material, std::function<bool()>&& upload) {
    material->last_used = ++use_counter;
    if (!async_custom_loading) {
        const bool was_unloaded = material->IsUnloaded();
        material->LoadFromDisk(flip_png_files);
        if (was_unloaded) {
            custom_tex_mem_usage.fetch_add(material->size * CUSTOM_TEX_MEM_FACTOR,
                                           std::memory_order_relaxed);
        }
        return upload();
    }
    if (material->IsUnloaded()) {
        material->state = DecodeState::Pending;
        workers->QueueWork([material, this] {
            material->LoadFromDisk(flip_png_files);
            custom_tex_mem_usage.fetch_add(material->size * CUSTOM_TEX_MEM_FACTOR,
                                           std::memory_order_relaxed);
        });
    }
    async_uploads.push_back({
        .material = material,
        .func = std::move(upload),
    });
    return false;
}

bool CustomTexManager::ReadConfig(u64 title_id, bool options_only) {
    const std::string load_path =
        fmt::format("{}textures/{:016X}/", GetUserPath(FileUtil::UserPath::LoadDir), title_id);
    if (!FileUtil::Exists(load_path)) {
        FileUtil::CreateFullPath(load_path);
    }

    const std::string config_path = load_path + "pack.json";
    FileUtil::IOFile config_file{config_path, "r"};
    if (!config_file.IsOpen()) {
        LOG_INFO(Render, "Unable to find pack config file, using legacy defaults");
        return false;
    }
    std::string config(config_file.GetSize(), '\0');
    const std::size_t read_size = config_file.ReadBytes(config.data(), config.size());
    if (!read_size) {
        return false;
    }

    nlohmann::json json = nlohmann::json::parse(config, nullptr, false, true);

    const auto& options = json["options"];
    skip_mipmap = options["skip_mipmap"].get<bool>();
    flip_png_files = options["flip_png_files"].get<bool>();
    use_new_hash = options["use_new_hash"].get<bool>();

    if (options_only) {
        return true;
    }

    const auto& textures = json["textures"];
    for (const auto& material : textures.items()) {
        std::size_t idx{};
        const u64 hash = std::stoull(material.key(), &idx, 16);
        if (!idx) {
            LOG_ERROR(Render, "Key {} is invalid, skipping", material.key());
            continue;
        }
        const auto parse = [&](const std::string& file) {
            const std::string filename{FileUtil::GetFilename(file)};
            auto [it, new_hash] = path_to_hash_map.try_emplace(filename);
            it->second.push_back(hash);
        };
        const auto value = material.value();
        if (value.is_string()) {
            const auto file = value.get<std::string>();
            parse(file);
        } else if (value.is_array()) {
            const auto files = value.get<std::vector<std::string>>();
            for (const std::string& file : files) {
                parse(file);
            }
        } else {
            LOG_ERROR(Render, "Material with key {} is invalid", material.key());
        }
    }
    return true;
}

std::vector<FileUtil::FSTEntry> CustomTexManager::GetTextures(u64 title_id) {
    const std::string load_path =
        fmt::format("{}textures/{:016X}/", GetUserPath(FileUtil::UserPath::LoadDir), title_id);
    if (!FileUtil::Exists(load_path)) {
        FileUtil::CreateFullPath(load_path);
    }

    FileUtil::FSTEntry texture_dir;
    std::vector<FileUtil::FSTEntry> textures;
    FileUtil::ScanDirectoryTree(load_path, texture_dir, 64);
    FileUtil::GetAllFilesFromNestedEntries(texture_dir, textures);
    return textures;
}

void CustomTexManager::CreateWorkers() {
    // Decoding one picture of a 4K pack keeps a worker busy for a good part of a second.
    // The workers used to be pinned to the main thread's core at its priority, so every
    // area entered took time from the thread that presents the frames. They go to the
    // cores nothing else is pinned to, where there are any (Switch 2), and run at low
    // priority everywhere: a picture arrives a little later, the frame is not held.
    std::size_t num_workers = std::max(std::thread::hardware_concurrency(), 2U) >> 1;
    std::vector<std::uint32_t> cores;
#ifdef __SWITCH__
    const std::uint64_t allowed = Common::Horizon::GetAllowedCoreMask();
    for (std::uint32_t core = Common::Horizon::CoreEmu + 1; core < 64; ++core) {
        if (allowed & (1ULL << core)) {
            cores.push_back(core);
        }
    }
    if (cores.empty()) {
        cores.push_back(Common::Horizon::CoreFrontend);
    } else {
        num_workers = std::min<std::size_t>(cores.size(), 4);
    }
    LOG_INFO(Render, "Custom textures: {} decode workers at low priority, first core {} of {} "
                     "(allowed core mask {:#x})",
             num_workers, cores.front(), cores.size(), allowed);
#endif
    workers.reset(new Common::ThreadWorker{num_workers, "Custom textures", {}, std::move(cores),
                                           Common::ThreadPriority::Low});
}

void CustomTexManager::ComputeMemoryBudget() {
    // Prefer the Horizon process memory pool.
    u64 sys_mem = Common::Horizon::GetTotalMemorySize();
    if (sys_mem == 0) {
        sys_mem = Common::GetMemInfo().total_physical_memory;
    }

    // Reserve headroom for the emulator core, GPU driver and guest RAM.
    const u64 recommended_min_mem = 2_GiB;
    max_custom_tex_mem = (sys_mem / 2 < recommended_min_mem) ? (sys_mem / 2)
                                                             : (sys_mem - recommended_min_mem);
    LOG_INFO(Render, "Custom texture memory budget: {} MiB", max_custom_tex_mem / 1_MiB);
}

} // namespace VideoCore
