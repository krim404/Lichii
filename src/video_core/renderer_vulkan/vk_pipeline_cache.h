// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include <vector>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/async_draw_policy.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_prebuild.h"
#include "video_core/renderer_vulkan/vk_shader_reuse.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/shader_cache.h"

namespace Core {
class System;
}

namespace Shader::IR {
struct Program;
}

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

struct ComputePipelineCacheKey {
    u64 unique_hash;
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;

    size_t Hash() const noexcept;

    bool operator==(const ComputePipelineCacheKey& rhs) const noexcept;

    bool operator!=(const ComputePipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }
};
static_assert(std::has_unique_object_representations_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<ComputePipelineCacheKey>);

} // namespace Vulkan

namespace std {

template <>
struct hash<Vulkan::ComputePipelineCacheKey> {
    size_t operator()(const Vulkan::ComputePipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};

} // namespace std

namespace Vulkan {

class ComputePipeline;
class DescriptorPool;
class Device;
class PipelineStatistics;
class RenderPassCache;
class Scheduler;

using VideoCommon::ShaderInfo;

struct ShaderPools {
    void ReleaseContents() {
        flow_block.ReleaseContents();
        block.ReleaseContents();
        inst.ReleaseContents();
    }

    Shader::ObjectPool<Shader::IR::Inst> inst{8192};
    Shader::ObjectPool<Shader::IR::Block> block{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};
};

struct ComputePipelineCacheEntry {
    std::unique_ptr<ComputePipeline> pipeline;
    std::string failure;
    /// Constant buffer slots that were null pointers at translation (see NullStoragePointerSlots)
    std::vector<std::pair<u32, u32>> null_pointer_slots;
    /// Constant buffer slots bound as storage buffer pointers (see MappedStoragePointerKeys)
    std::vector<u64> mapped_pointers;
    u32 rebuilds{};

    [[nodiscard]] ComputePipeline* Get(bool native, GPUVAddr address,
                                       const ComputePipelineCacheKey& key) const;
};

class PipelineCache : public VideoCommon::ShaderCache {
public:
    explicit PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device,
                           Scheduler& scheduler, DescriptorPool& descriptor_pool,
                           GuestDescriptorQueue& guest_descriptor_queue,
                           DescriptorBufferRing& descriptor_buffer_ring,
                           RenderPassCache& render_pass_cache, BufferCache& buffer_cache,
                           TextureCache& texture_cache, VideoCore::ShaderNotify& shader_notify_);
    ~PipelineCache();

    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipeline();

    void TickFrame();

    [[nodiscard]] ComputePipeline* CurrentComputePipeline();

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

private:
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineSlowPath();

    /// Returns `pipeline` when it is built or its draw must wait for it, nullptr when the draw
    /// may be skipped while it builds (asynchronous shaders, CanDiscardPendingDraw).
    [[nodiscard]] GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline) noexcept;

    /// How the current draw treats an unbuilt pipeline (native_async_draws, CanDiscardPendingDraw).
    struct PendingDrawPolicy {
        bool native;
        NativePendingMode mode;
        bool can_discard;
    };
    [[nodiscard]] PendingDrawPolicy CurrentPendingDrawPolicy() const;

    /// Charges the time since `begin` to this frame's native wait budget.
    void ChargeNativeWait(std::chrono::steady_clock::time_point begin);

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline();

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
        bool build_in_parallel, std::shared_ptr<ShaderPools> emit_pools = nullptr,
        std::unique_ptr<EmissionBudget::Reservation> reservation = nullptr,
        bool allow_translation = true);

    using GraphicsEnvironmentArray =
        std::array<VideoCommon::GraphicsEnvironment, Maxwell::MaxShaderProgram>;

    /// A graphics pipeline translated on a translation worker, waiting for the GPU thread.
    struct TranslatedGraphicsPipeline {
        GraphicsPipelineCacheKey key;
        std::unique_ptr<GraphicsPipeline> pipeline; ///< Null when it is skipped or failed.
        GraphicsEnvironmentArray envs;              ///< For the disk cache.
        /// The detached environments could not answer translation (DetachedEnvironmentMiss).
        bool translate_on_gpu_thread{};
    };

    void SerializeGraphicsPipeline(const GraphicsPipelineCacheKey& key,
                                   GraphicsEnvironmentArray envs);

    /// Whether graphics_key's new pipeline is translated on a translation worker.
    [[nodiscard]] bool CanTranslateAsynchronously() const;

    /// Builds graphics_key's pipeline from shaders an earlier pipeline translated, null if none fit.
    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipelineFromReusedShaders(
        GraphicsEnvironments& environments);

    /// Detaches the environments of graphics_key's programs from the engine and guest memory.
    /// False when one cannot be (the pipeline is then translated on the GPU thread).
    [[nodiscard]] bool DetachGraphicsEnvironments(GraphicsEnvironmentArray& envs);

    /// Queues the translation of graphics_key's pipeline on a translation worker and waits for
    /// it as WaitForGraphicsTranslation does.
    bool QueueGraphicsTranslation(GraphicsEnvironmentArray envs);

    /// Waits for a queued translation as the current draw's pending policy allows: until it is
    /// done for a draw that cannot be skipped, within the native wait budget for a bounded one.
    /// True when the translation finished and its result was adopted.
    bool WaitForGraphicsTranslation(std::shared_future<void> translated);

    /// Runs on a translation worker thread.
    void TranslateGraphicsPipeline(const GraphicsPipelineCacheKey& key,
                                   GraphicsEnvironmentArray envs, std::promise<void> done);

    /// Moves the pipelines finished by the translation workers into graphics_cache (GPU thread).
    void AdoptTranslatedGraphicsPipelines();

    /// Translated shaders of a graphics pipeline.
    struct GraphicsShaders {
        /// Stage infos and, unless emit_stages builds them, modules; what shader_reuse gets.
        std::shared_ptr<ReusableShaders> shaders;
        /// Into shaders->infos, null for stages without a program.
        std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};
        std::function<std::shared_ptr<const GraphicsShaderModules>()> emit_stages;
        std::optional<MeshEmulationSetup> mesh_emulation;
    };

    /// Shaders of an earlier graphics pipeline that fit `envs` (vk_shader_reuse.h), or null.
    [[nodiscard]] std::shared_ptr<const ReusableShaders> FindReusableShaders(
        const ShaderTranslationKey& key, std::span<Shader::Environment* const> envs,
        u64 pipeline_hash);

    /// With `verify_against`, logs the stages whose SPIR-V differs from those shaders'.
    [[nodiscard]] std::optional<GraphicsShaders> BuildGraphicsShaders(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        const ShaderTranslationKey& translation_key, std::span<Shader::Environment* const> envs,
        const ReusableShaders* verify_against, bool build_in_parallel,
        std::shared_ptr<ShaderPools> emit_pools,
        std::unique_ptr<EmissionBudget::Reservation> reservation);

    [[nodiscard]] bool StaleNullPointerSlot(const ComputePipelineCacheEntry& entry) const;

    /// Translates the current compute program, binding the pointers in keep_pointers whatever
    /// their slots hold now (see GenericEnvironment::KeepStoragePointers)
    ComputePipelineCacheEntry CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                    const ShaderInfo* shader,
                                                    std::span<const u64> keep_pointers = {});

    ComputePipelineCacheEntry CreateComputePipeline(ShaderPools& pools,
                                                     const ComputePipelineCacheKey& key,
                                                     Shader::Environment& env,
                                                     PipelineStatistics* statistics,
                                                     bool build_in_parallel);

    void SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                      const vk::PipelineCache& pipeline_cache, u32 cache_version);

    vk::PipelineCache LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                              u32 expected_cache_version);

    void QueueVulkanPipelineCacheFlush();

    const Device& device;
    Scheduler& scheduler;
    DescriptorPool& descriptor_pool;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;
    RenderPassCache& render_pass_cache;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    VideoCore::ShaderNotify& shader_notify;
    bool use_asynchronous_shaders{};
    bool use_vulkan_pipeline_cache{};
    const bool use_shader_reuse;
    const size_t num_pipeline_workers;
    EmissionBudget emission_budget;

    GraphicsPipelineCacheKey graphics_key{};
    GraphicsPipeline* current_pipeline{};

    /// Destroys graphics pipelines unused for a while, once the GPU is done with them.
    void EvictUnusedPipelines();
    u64 frame_count{};
    /// Wait budget of native draws for unbuilt pipelines left in this frame, in microseconds
    /// (native_async_draws = Bounded). Only touched on the GPU thread; reset in TickFrame.
    s64 native_wait_budget_us{8000};
    s64 native_wait_used_us{}; ///< Budget spent since the last 10 s log line.
    std::deque<std::pair<u64, std::unique_ptr<GraphicsPipeline>>> evicted_pipelines; ///< (tick, pipeline)
    std::unordered_set<u64> used_and_evicted; ///< Key hashes of pipelines used this session, evicted since

    ::Common::unordered_map<ComputePipelineCacheKey, ComputePipelineCacheEntry> compute_cache;
    /// Replaced compute pipelines, destroyed once the GPU and their queued build are done with
    /// them (tick, pipeline)
    std::deque<std::pair<u64, std::unique_ptr<ComputePipeline>>> retired_compute_pipelines;
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> graphics_cache;

    ShaderPools main_pools;
    std::mutex spare_pools_mutex;
    std::vector<std::shared_ptr<ShaderPools>> spare_pools;
    ShaderReuseCache shader_reuse;

    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;

    std::filesystem::path pipeline_cache_filename;

    /// Key hashes of the graphics pipelines in the disk cache. Only those a recent session used
    /// (used_pipelines) are built at boot; the rest are built when first needed, and not stored
    /// again. Each built pipeline holds driver memory (title 1: 6.5k cached, a quarter used).
    std::unordered_set<u64> disk_pipelines;
    std::unordered_map<u64, u8> used_pipelines; ///< Key hash -> sessions since it was last used
    std::unordered_set<const GraphicsPipeline*> used_this_session;
    std::filesystem::path used_pipelines_filename;
    std::chrono::steady_clock::time_point used_pipelines_saved{};
    void SaveUsedPipelines();

    std::filesystem::path vulkan_pipeline_cache_filename;
    vk::PipelineCache vulkan_pipeline_cache;
    size_t pipelines_since_flush{};
    std::chrono::steady_clock::time_point last_flush{};
    std::atomic<size_t> last_cache_size{};
    std::atomic_bool flush_in_flight{};

    Common::ThreadWorker workers;
    Common::ThreadWorker serialization_thread;
    DynamicFeatures dynamic_features;
    PipelinePrebuild<GraphicsPipelineCacheKey, GraphicsPipeline> background_pipelines;

    /// Keys whose pipeline is being translated on a translation worker, with the future that
    /// becomes ready when the result waits for adoption (GPU thread only).
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::shared_future<void>>
        translating_graphics;
    /// Finished translations, handed from the translation workers to the GPU thread.
    std::mutex translated_mutex;
    std::vector<TranslatedGraphicsPipeline> translated_graphics;
    std::atomic<size_t> num_translated{};
    /// Keys whose translation needs more than detached environments hold (GPU thread only).
    ::Common::unordered_set<GraphicsPipelineCacheKey> synchronous_graphics;
    /// Set when the cache is destroyed: queued translations return without translating.
    std::atomic_bool stop_translations{};
    /// Zero without asynchronous shaders, or on drivers that break with parallel compiles.
    const size_t num_translation_threads;
    /// Declared last so it is destroyed first: its threads finish the translation they are
    /// running (and drop the queued ones) while everything they use is still alive.
    Common::ThreadWorker translation_workers;
};

} // namespace Vulkan
