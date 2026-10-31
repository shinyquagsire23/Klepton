// Pipeline-object hooks: what the Vulkan layer (kl_vulkan.c) tells an add-on
// after each successful create, with the guest's own create info — for the
// pipeline recorder and replayer in kl_fossilize.cpp. Include after the Vulkan
// headers.
//
// The shape is Glovebox's (github.com/glovebox-vr/glovebox, MIT,
// klepton/runtime/gfx/kl_vk_pipe_hooks.h), which is where recording and
// replaying a Source 2 game's pipelines on this platform was worked out.
//
// device_created hands over the layer's own vkGetDeviceProcAddr, so a replayer
// creates its objects through exactly the path the guest does: the same
// fix-ups, the same MoltenVK state, and so the same MSL — which is what the
// system's Metal compiler cache is keyed on.
#ifndef KL_VK_PIPE_HOOKS_H
#define KL_VK_PIPE_HOOKS_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct klvk_pipe_hooks {
    void (*device_created)(VkDevice dev, PFN_vkGetDeviceProcAddr layer_gdpa);
    void (*sampler)(VkSampler h, const VkSamplerCreateInfo *ci);
    void (*set_layout)(VkDescriptorSetLayout h, const VkDescriptorSetLayoutCreateInfo *ci);
    void (*pipeline_layout)(VkPipelineLayout h, const VkPipelineLayoutCreateInfo *ci);
    void (*shader_module)(VkShaderModule h, const VkShaderModuleCreateInfo *ci);
    void (*render_pass)(VkRenderPass h, const VkRenderPassCreateInfo *ci);
    void (*render_pass2)(VkRenderPass h, const VkRenderPassCreateInfo2 *ci);
    void (*graphics_pipeline)(VkPipeline h, const VkGraphicsPipelineCreateInfo *ci);
    void (*compute_pipeline)(VkPipeline h, const VkComputePipelineCreateInfo *ci);
    // The guest's device create info, as it asked, just before the device is
    // made (its features go into the recording).
    void (*device_info)(const VkDeviceCreateInfo *ci);
} klvk_pipe_hooks;

void klvk_set_pipe_hooks(const klvk_pipe_hooks *h);

// On a replayer's own threads: their creates are not handed to the hooks, are
// compiled where they are asked for (never behind the frame) and are not
// counted as the guest's compiles.
void klvk_set_replay_thread(int on);

// Whether the newest instance is the one KL_VK_FOZ_APP names, by its
// application or its engine name (unset: every instance is). A process can
// hold more than one device — an OpenVR runtime brings up a throwaway one
// before the game's — and only the game's is worth recording and replaying on.
int klvk_instance_wanted(void);

#ifdef __cplusplus
}
#endif
#endif
