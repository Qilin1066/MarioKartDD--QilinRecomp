// What is done to the picture before it's shown through its look
// (display.cpp): SMAA 1x anti-aliasing at the picture's own size, then AMD
// FSR 1 up to the size it's shown at (EASU scales, RCAS sharpens). Both are
// off unless the settings ask; nothing is made until one is first used.
//
// The shaders are shaders/post/smaa.glsl and shaders/post/fsr.comp over the authors'
// own sources (shaders/post/third_party.txt), compiled into post_shaders.inc.
#pragma once

#include <cstdint>

#include "qilin/host/vulkan.hpp"

namespace qilin::host {

class Post {
public:
    void create(const Gpu& gpu);
    void destroy();
    // FSR's passes are compute shaders: not on a device whose queue has none.
    bool fsr_available() const { return compute_; }

    struct Result {
        VkImageView view;
        uint32_t width, height;
    };
    // With nothing of ours in flight on the GPU: what the next record() will
    // do to `source` (a view of a `w` x `h` picture), and the view and size
    // the look then samples. `fsr`: up to `out_w` x `out_h`.
    Result prepare(VkImageView source, uint32_t w, uint32_t h, bool smaa, bool fsr, uint32_t out_w, uint32_t out_h, float sharpness);
    // The passes, into `cmd`: the source must be in SHADER_READ_ONLY_OPTIMAL
    // layout; the result is left in it.
    void record(VkCommandBuffer cmd);

private:
    void make_shared();
    void make_smaa();
    void make_fsr();

    const Gpu* gpu_ = nullptr;
    bool compute_ = false;
    VkSampler linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    // SMAA: edges, blending weights, the result.
    VkDescriptorSetLayout smaa_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout smaa_layout_ = VK_NULL_HANDLE;
    VkPipeline smaa_pipelines_[3] = {};
    VkDescriptorSet smaa_sets_[3] = {};
    Image smaa_images_[3];
    Image area_, search_;  // its lookup tables
    Buffer tables_;        // until they're uploaded (the first record)
    bool tables_uploaded_ = false;
    // FSR: EASU's output, RCAS's.
    VkDescriptorSetLayout fsr_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout fsr_layout_ = VK_NULL_HANDLE;
    VkPipeline fsr_pipelines_[2] = {};
    VkDescriptorSet fsr_sets_[2] = {};
    Image fsr_images_[2];
    // The next record().
    bool smaa_ = false, fsr_ = false;
    uint32_t w_ = 0, h_ = 0;
    float sharpness_ = 0.8f;
};

}  // namespace qilin::host
