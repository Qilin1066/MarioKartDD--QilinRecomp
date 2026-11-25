// SMAA and FSR 1 before the picture is shown (post.hpp).
//
// One present is in flight at a time and prepare() runs between two, so the
// descriptor sets are rewritten and the images remade in place.
#include "post.hpp"

#include <cstring>
#include <span>
#include <vector>

#include "fsr_constants.hpp"
#include "post_shaders.inc"

namespace qilin::host {

namespace {
// SMAA's area and search tables (iryoku/smaa, MIT), fetched when the build is configured.
namespace tables {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}  // namespace tables

constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kAreaW = 160, kAreaH = 560, kSearchW = 64, kSearchH = 16;

VkShaderModule module(const Gpu& gpu, std::span<const uint32_t> code) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size_bytes();
    ci.pCode = code.data();
    VkShaderModule m;
    check(vkCreateShaderModule(gpu.device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

void write_image(const Gpu& gpu, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkSampler sampler, VkImageView view,
                 VkImageLayout layout) {
    const VkDescriptorImageInfo ii{sampler, view, layout};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(gpu.device, 1, &w, 0, nullptr);
}
}  // namespace

void Post::create(const Gpu& gpu) {
    gpu_ = &gpu;
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &n, families.data());
    compute_ = gpu.family < n && (families[gpu.family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
}

void Post::destroy() {
    if (!gpu_ || !gpu_->device) return;
    const VkDevice dev = gpu_->device;
    for (Image& im : smaa_images_) im.destroy(*gpu_);
    for (Image& im : fsr_images_) im.destroy(*gpu_);
    area_.destroy(*gpu_), search_.destroy(*gpu_), tables_.destroy(*gpu_);
    for (VkPipeline p : smaa_pipelines_)
        if (p) vkDestroyPipeline(dev, p, nullptr);
    for (VkPipeline p : fsr_pipelines_)
        if (p) vkDestroyPipeline(dev, p, nullptr);
    if (smaa_layout_) vkDestroyPipelineLayout(dev, smaa_layout_, nullptr);
    if (fsr_layout_) vkDestroyPipelineLayout(dev, fsr_layout_, nullptr);
    if (smaa_set_layout_) vkDestroyDescriptorSetLayout(dev, smaa_set_layout_, nullptr);
    if (fsr_set_layout_) vkDestroyDescriptorSetLayout(dev, fsr_set_layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (linear_) vkDestroySampler(dev, linear_, nullptr);
    if (nearest_) vkDestroySampler(dev, nearest_, nullptr);
    *this = Post{};
}

// The pool and the samplers, for both.
void Post::make_shared() {
    const Gpu& gpu = *gpu_;
    if (!pool_) {
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * 3 + 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 5;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(gpu.device, &dpi, nullptr, &pool_), "vkCreateDescriptorPool");
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(gpu.device, &sci, nullptr, &linear_), "vkCreateSampler");
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        check(vkCreateSampler(gpu.device, &sci, nullptr, &nearest_), "vkCreateSampler");
    }
}

// SMAA's layout, pipelines, sets and tables.
void Post::make_smaa() {
    const Gpu& gpu = *gpu_;
    if (smaa_layout_) return;
    VkDescriptorSetLayoutBinding b[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 3;
    sl.pBindings = b;
    check(vkCreateDescriptorSetLayout(gpu.device, &sl, nullptr, &smaa_set_layout_), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &smaa_set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(gpu.device, &pl, nullptr, &smaa_layout_), "vkCreatePipelineLayout");
    const std::span<const uint32_t> code[3][2] = {{kSmaa1Vert, kSmaa1Frag}, {kSmaa2Vert, kSmaa2Frag}, {kSmaa3Vert, kSmaa3Frag}};
    for (uint32_t pass = 0; pass < 3; ++pass) {
        VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                  {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        for (int i = 0; i < 2; ++i) {
            stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module = module(gpu, code[pass][i]);
            stages[i].pName = "main";
        }
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynamic;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &kFormat;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pColorBlendState = &cb;
        info.pDynamicState = &dyn;
        info.layout = smaa_layout_;
        check(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &smaa_pipelines_[pass]), "vkCreateGraphicsPipelines");
        for (const auto& s : stages) vkDestroyShaderModule(gpu.device, s.module, nullptr);
    }
    const VkDescriptorSetLayout layouts[3] = {smaa_set_layout_, smaa_set_layout_, smaa_set_layout_};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 3;
    ai.pSetLayouts = layouts;
    check(vkAllocateDescriptorSets(gpu.device, &ai, smaa_sets_), "vkAllocateDescriptorSets");
    // The tables: the area texture (two channels) and the search texture (one), uploaded by the first record().
    area_ = Image::create(gpu, kAreaW, kAreaH, VK_FORMAT_R8G8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    search_ = Image::create(gpu, kSearchW, kSearchH, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    static_assert(sizeof tables::areaTexBytes == size_t{kAreaW} * kAreaH * 2 && sizeof tables::searchTexBytes == size_t{kSearchW} * kSearchH);
    tables_ = Buffer::host_visible(gpu, sizeof tables::areaTexBytes + sizeof tables::searchTexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(tables_.mapped, tables::areaTexBytes, sizeof tables::areaTexBytes);
    std::memcpy(static_cast<uint8_t*>(tables_.mapped) + sizeof tables::areaTexBytes, tables::searchTexBytes, sizeof tables::searchTexBytes);
    tables_uploaded_ = false;
}

void Post::make_fsr() {
    const Gpu& gpu = *gpu_;
    if (fsr_layout_) return;
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    for (auto& x : b) x.descriptorCount = 1, x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 2;
    sl.pBindings = b;
    check(vkCreateDescriptorSetLayout(gpu.device, &sl, nullptr, &fsr_set_layout_), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &fsr_set_layout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(gpu.device, &pl, nullptr, &fsr_layout_), "vkCreatePipelineLayout");
    const std::span<const uint32_t> code[2] = {kFsrEasu, kFsrRcas};
    for (int i = 0; i < 2; ++i) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module(gpu, code[i]);
        ci.stage.pName = "main";
        ci.layout = fsr_layout_;
        check(vkCreateComputePipelines(gpu.device, VK_NULL_HANDLE, 1, &ci, nullptr, &fsr_pipelines_[i]), "vkCreateComputePipelines");
        vkDestroyShaderModule(gpu.device, ci.stage.module, nullptr);
    }
    const VkDescriptorSetLayout layouts[2] = {fsr_set_layout_, fsr_set_layout_};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    check(vkAllocateDescriptorSets(gpu.device, &ai, fsr_sets_), "vkAllocateDescriptorSets");
}

Post::Result Post::prepare(VkImageView source, uint32_t w, uint32_t h, bool smaa, bool fsr, uint32_t out_w, uint32_t out_h, float sharpness) {
    const Gpu& gpu = *gpu_;
    fsr = fsr && compute_ && out_w && out_h;
    smaa_ = smaa, fsr_ = fsr, w_ = w, h_ = h, sharpness_ = sharpness;
    Result r{source, w, h};
    if (!smaa && !fsr) return r;
    make_shared();
    constexpr VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    constexpr VkDescriptorType sampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    if (smaa) {
        make_smaa();
        if (smaa_images_[0].width != w || smaa_images_[0].height != h)
            for (Image& im : smaa_images_) {
                im.destroy(gpu);
                im = Image::create(gpu, w, h, kFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            }
        // What each pass reads: the colour; the edges, the area and search tables; the colour and the weights.
        const VkImageView inputs[3][3] = {{source, area_.view, search_.view},
                                          {smaa_images_[0].view, area_.view, search_.view},
                                          {source, smaa_images_[1].view, search_.view}};
        for (uint32_t pass = 0; pass < 3; ++pass)
            for (uint32_t i = 0; i < 3; ++i) write_image(gpu, smaa_sets_[pass], i, sampled, i == 2 ? nearest_ : linear_, inputs[pass][i], read);
        r.view = smaa_images_[2].view;
    }
    if (fsr) {
        make_fsr();
        if (fsr_images_[0].width != out_w || fsr_images_[0].height != out_h)
            for (Image& im : fsr_images_) {
                im.destroy(gpu);
                im = Image::create(gpu, out_w, out_h, kFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            }
        const VkImageView in[2] = {r.view, fsr_images_[0].view};
        for (uint32_t pass = 0; pass < 2; ++pass) {
            write_image(gpu, fsr_sets_[pass], 0, sampled, linear_, in[pass], read);
            write_image(gpu, fsr_sets_[pass], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE, fsr_images_[pass].view, VK_IMAGE_LAYOUT_GENERAL);
        }
        r = {fsr_images_[1].view, out_w, out_h};
    }
    return r;
}

void Post::record(VkCommandBuffer cmd) {
    constexpr VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (smaa_) {
        if (!tables_uploaded_) {
            const struct {
                Image* image;
                VkDeviceSize offset;
            } uploads[] = {{&area_, 0}, {&search_, sizeof tables::areaTexBytes}};
            for (const auto& u : uploads) {
                transition(cmd, u.image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy region{};
                region.bufferOffset = u.offset;
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {u.image->width, u.image->height, 1};
                vkCmdCopyBufferToImage(cmd, tables_.buffer, u.image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                transition(cmd, u.image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, read);
            }
            tables_uploaded_ = true;
        }
        const float metrics[4] = {1.0f / float(w_), 1.0f / float(h_), float(w_), float(h_)};
        for (uint32_t pass = 0; pass < 3; ++pass) {
            Image& out = smaa_images_[pass];
            transition(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ca.imageView = out.view;
            ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ca.loadOp = pass == 2 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;  // edge detection discards the rest
            ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {out.width, out.height}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &ca;
            vkCmdBeginRendering(cmd, &ri);
            const VkViewport viewport{0, 0, float(out.width), float(out.height), 0, 1};
            const VkRect2D scissor{{0, 0}, {out.width, out.height}};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, smaa_pipelines_[pass]);
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, smaa_layout_, 0, 1, &smaa_sets_[pass], 0, nullptr);
            vkCmdPushConstants(cmd, smaa_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof metrics, metrics);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            transition(cmd, out.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, read);
        }
    }
    if (fsr_) {
        for (uint32_t pass = 0; pass < 2; ++pass) {
            Image& out = fsr_images_[pass];
            transition(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            uint32_t push[20] = {};
            if (pass == 0) fsr::easu_constants(w_, h_, out.width, out.height, push);
            else fsr::rcas_constants(sharpness_, push);
            push[16] = out.width, push[17] = out.height;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fsr_pipelines_[pass]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fsr_layout_, 0, 1, &fsr_sets_[pass], 0, nullptr);
            vkCmdPushConstants(cmd, fsr_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
            vkCmdDispatch(cmd, (out.width + 15) / 16, (out.height + 15) / 16, 1);  // a 16x16 tile a group
            transition(cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, read);
        }
    }
}

}  // namespace qilin::host
