#include "graphics.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Здесь находятся буферы, дескрипторы, загрузка шейдеров и команды Vulkan.
namespace graphics {
namespace {

using application::ConeVertex;

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
};

Buffer vertex_buffer, index_buffer, scene_buffer, model_buffer;
Buffer point_buffer, spot_buffer, marker_buffer, line_buffer;
uint32_t index_count = 0;
VkDeviceSize max_storage_range = 0;
VkDescriptorSetLayout descriptor_layouts[2]{};
VkDescriptorSet descriptor_sets[2]{};
VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
VkPipeline pipeline = VK_NULL_HANDLE, marker_pipeline = VK_NULL_HANDLE, line_pipeline = VK_NULL_HANDLE;

bool check(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) {
        return true;
    }
    std::cerr << operation << " failed (VkResult " << result << ")\n";
    return false;
}

std::filesystem::path shaderDirectory() {
#ifdef _WIN32
    std::vector<wchar_t> executable(32768);
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
    if (length == 0 || length >= executable.size()) {
        throw std::runtime_error("Cannot determine the executable directory");
    }
    return std::filesystem::path(std::wstring(executable.data(), length)).parent_path() / L"shaders";
#else
    return std::filesystem::current_path() / "shaders";
#endif
}

bool loadShader(const std::filesystem::path& path, VkShaderModule& module) {
    const auto utf8 = path.u8string();
    const std::string printable(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        std::cerr << "Cannot open shader: " << printable << ". Run build.cmd to compile shaders.\n";
        return false;
    }
    const auto bytes = input.tellg();
    if (bytes < 20 || bytes % 4 != 0 || bytes > 4 * 1024 * 1024) {
        std::cerr << "Invalid SPIR-V shader size: " << printable << '\n';
        return false;
    }
    std::vector<uint32_t> code(size_t(bytes) / sizeof(uint32_t));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(code.data()), std::streamsize(bytes)) || code[0] != 0x07230203u) {
        std::cerr << "Cannot read valid SPIR-V shader: " << printable << '\n';
        return false;
    }
    const VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size() * sizeof(uint32_t),
        .pCode = code.data(),
    };
    return check(vkCreateShaderModule(graphics::internal::context.device, &info, nullptr, &module),
                 "Create shader module");
}

bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data, Buffer& buffer) {
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo allocation_info = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VmaAllocationInfo result{};
    const auto allocator = graphics::internal::context.allocator;
    if (!check(vmaCreateBuffer(allocator, &info, &allocation_info, &buffer.handle, &buffer.allocation, &result),
               "Create mapped buffer")) {
        return false;
    }
    buffer.mapped = result.pMappedData;
    buffer.capacity = size;
    if (!buffer.mapped) {
        std::cerr << "Mapped buffer has no host address\n";
        return false;
    }
    if (data) {
        std::memcpy(buffer.mapped, data, size_t(size));
        // VMA handles atom-size alignment; this is a no-op on coherent memory.
        return check(vmaFlushAllocation(allocator, buffer.allocation, 0, size), "Flush buffer");
    }
    return true;
}

void destroyBuffer(Buffer& buffer) {
    if (buffer.handle != VK_NULL_HANDLE)
        vmaDestroyBuffer(internal::context.allocator, buffer.handle, buffer.allocation);
    buffer = {};
}

bool writeBuffer(Buffer& buffer, const void* data, VkDeviceSize size) {
    if (size == 0) return true;
    std::memcpy(buffer.mapped, data, size_t(size));
    return check(vmaFlushAllocation(internal::context.allocator, buffer.allocation, 0, size), "Flush buffer");
}

// Вызывается после ожидания fence предыдущего кадра в internal::prepare().
bool ensureBufferSize(Buffer& buffer, VkDeviceSize needed, VkBufferUsageFlags usage) {
    if (needed <= buffer.capacity) return true;
    VkDeviceSize capacity = std::max(needed, buffer.capacity * 2);
    if (usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
        if (needed > max_storage_range) {
            std::cerr << "Light buffer exceeds maxStorageBufferRange\n";
            return false;
        }
        capacity = std::min(capacity, max_storage_range);
    }
    Buffer replacement;
    if (!createBuffer(capacity, usage, nullptr, replacement)) {
        destroyBuffer(replacement);
        return false;
    }
    destroyBuffer(buffer);
    buffer = replacement;
    return true;
}

void updateDescriptors() {
    const VkDescriptorBufferInfo infos[] = {
        {scene_buffer.handle, 0, sizeof(SceneUniform)},
        {point_buffer.handle, 0, point_buffer.capacity},
        {spot_buffer.handle, 0, spot_buffer.capacity},
        {model_buffer.handle, 0, sizeof(ModelUniform)}
    };
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptor_sets[i == 3 ? 1 : 0];
        writes[i].dstBinding = i == 3 ? 0 : i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = (i == 1 || i == 2) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(internal::context.device, 4, writes, 0, nullptr);
}

bool createDescriptors() {
    const auto device = internal::context.device;
    const VkDescriptorSetLayoutBinding scene_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}
    };
    const VkDescriptorSetLayoutBinding model_binding{
        0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr
    };
    for (int i = 0; i < 2; ++i) {
        const VkDescriptorSetLayoutCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = i == 0 ? 3u : 1u,
            .pBindings = i == 0 ? scene_bindings : &model_binding
        };
        if (!check(vkCreateDescriptorSetLayout(device, &info, nullptr, &descriptor_layouts[i]), "Create descriptor layout"))
            return false;
    }
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}
    };
    const VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 2, .pPoolSizes = sizes
    };
    if (!check(vkCreateDescriptorPool(device, &pool, nullptr, &descriptor_pool), "Create descriptor pool")) return false;
    const VkDescriptorSetAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool, .descriptorSetCount = 2, .pSetLayouts = descriptor_layouts
    };
    if (!check(vkAllocateDescriptorSets(device, &allocate, descriptor_sets), "Allocate descriptors")) return false;
    updateDescriptors();
    const VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2, .pSetLayouts = descriptor_layouts
    };
    return check(vkCreatePipelineLayout(device, &layout, nullptr, &pipeline_layout), "Create pipeline layout");
}

bool createPipeline(const char* shader, bool marker, VkPrimitiveTopology topology, VkPipeline& output) {
    const auto& context = graphics::internal::context;
    const auto shader_directory = shaderDirectory();
    struct Shader {
        VkShaderModule handle = VK_NULL_HANDLE;
        ~Shader() {
            vkDestroyShaderModule(graphics::internal::context.device, handle, nullptr);
        }
    };
    Shader vertex;
    Shader fragment;
    if (!loadShader(shader_directory / (std::string(shader) + ".vert.spv"), vertex.handle)) {
        return false;
    }
    if (!loadShader(shader_directory / (std::string(shader) + ".frag.spv"), fragment.handle)) {
        return false;
    }
    const VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex.handle, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment.handle, .pName = "main"},
    };
    const VkVertexInputBindingDescription vertex_binding{0, sizeof(ConeVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ConeVertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ConeVertex, color)},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ConeVertex, normal)},
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vertex_binding,
        .vertexAttributeDescriptionCount = marker ? 2u : 3u, .pVertexAttributeDescriptions = attributes,
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topology,
    };
    const VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = marker ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    const VkPipelineColorBlendAttachmentState color_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &color_attachment,
    };
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dynamic_states,
    };
    const VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport, .pRasterizationState = &rasterization,
        .pMultisampleState = &multisampling, .pDepthStencilState = &depth,
        .pColorBlendState = &blend, .pDynamicState = &dynamic,
        .layout = pipeline_layout, .renderPass = context.render_pass,
    };
    return check(vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &info, nullptr, &output),
                 "Create graphics pipeline");
}

} // namespace

bool initializeCone(const application::ConeMesh& mesh) {
    try {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(internal::context.physical_device, &properties);
        max_storage_range = properties.limits.maxStorageBufferRange;
        index_count = uint32_t(mesh.indices.size());
        if (createBuffer(mesh.vertices.size() * sizeof(ConeVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, mesh.vertices.data(), vertex_buffer) &&
            createBuffer(mesh.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mesh.indices.data(), index_buffer) &&
            createBuffer(sizeof(SceneUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, scene_buffer) &&
            createBuffer(sizeof(ModelUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, nullptr, model_buffer) &&
            createBuffer(sizeof(PointLightGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr, point_buffer) &&
            createBuffer(sizeof(SpotLightGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr, spot_buffer) &&
            createDescriptors() &&
            createPipeline("cone", false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, pipeline) &&
            createPipeline("marker", true, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, marker_pipeline) &&
            createPipeline("marker", true, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, line_pipeline)) return true;
    } catch (const std::exception& error) {
        std::cerr << "Resource initialization failed: " << error.what() << '\n';
    }
    shutdownCone();
    return false;
}

void shutdownCone() {
    const auto& context = internal::context;
    if (context.device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(context.device);
    for (VkPipeline* p : {&pipeline, &marker_pipeline, &line_pipeline}) {
        vkDestroyPipeline(context.device, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    vkDestroyPipelineLayout(context.device, pipeline_layout, nullptr);
    vkDestroyDescriptorPool(context.device, descriptor_pool, nullptr);
    for (int i = 0; i < 2; ++i) {
        vkDestroyDescriptorSetLayout(context.device, descriptor_layouts[i], nullptr);
        descriptor_layouts[i] = VK_NULL_HANDLE;
        descriptor_sets[i] = VK_NULL_HANDLE;
    }
    for (Buffer* b : {&vertex_buffer, &index_buffer, &scene_buffer, &model_buffer,
                      &point_buffer, &spot_buffer, &marker_buffer, &line_buffer}) destroyBuffer(*b);
    pipeline_layout = VK_NULL_HANDLE;
    descriptor_pool = VK_NULL_HANDLE;
    index_count = 0;
}

bool renderScene(const internal::FrameData& fd, const SceneUniform& scene, const ModelUniform& model,
                 const std::vector<PointLightGpu>& points, const std::vector<SpotLightGpu>& spots,
                 const application::LightMarkers& markers) {
    const auto& context = internal::context;
    if (fd.command_buffer == VK_NULL_HANDLE || fd.framebuffer == VK_NULL_HANDLE ||
        context.swapchain_extent.width == 0 || context.swapchain_extent.height == 0) return false;
    if (!check(vkResetCommandBuffer(fd.command_buffer, 0), "Reset command buffer")) return false;

    // При пустом массиве буфер всё ещё содержит место для одного элемента, а счётчик равен нулю.
    const VkDeviceSize point_bytes = points.size() * sizeof(PointLightGpu);
    const VkDeviceSize spot_bytes = spots.size() * sizeof(SpotLightGpu);
    const VkDeviceSize marker_bytes = markers.triangles.size() * sizeof(ConeVertex);
    const VkDeviceSize line_bytes = markers.lines.size() * sizeof(ConeVertex);
    if (!ensureBufferSize(point_buffer, point_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !ensureBufferSize(spot_buffer, spot_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !ensureBufferSize(marker_buffer, marker_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !ensureBufferSize(line_buffer, line_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return false;
    updateDescriptors();
    if (!writeBuffer(scene_buffer, &scene, sizeof(scene)) || !writeBuffer(model_buffer, &model, sizeof(model)) ||
        !writeBuffer(point_buffer, points.data(), point_bytes) || !writeBuffer(spot_buffer, spots.data(), spot_bytes) ||
        !writeBuffer(marker_buffer, markers.triangles.data(), marker_bytes) ||
        !writeBuffer(line_buffer, markers.lines.data(), line_bytes)) return false;

    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    if (!check(vkBeginCommandBuffer(fd.command_buffer, &begin), "Begin command buffer")) return false;
    const VkClearValue clear[] = {
        {.color = {{0.025f, 0.035f, 0.055f, 1.0f}}}, {.depthStencil = {1.0f, 0}}
    };
    const VkRenderPassBeginInfo render_pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = context.render_pass, .framebuffer = fd.framebuffer,
        .renderArea = {.extent = context.swapchain_extent}, .clearValueCount = 2, .pClearValues = clear
    };
    vkCmdBeginRenderPass(fd.command_buffer, &render_pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, float(context.swapchain_extent.width), float(context.swapchain_extent.height), 0, 1};
    const VkRect2D scissor{{0, 0}, context.swapchain_extent};
    vkCmdSetViewport(fd.command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(fd.command_buffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                            0, 2, descriptor_sets, 0, nullptr);
    vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &vertex_buffer.handle, &offset);
    vkCmdBindIndexBuffer(fd.command_buffer, index_buffer.handle, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(fd.command_buffer, index_count, 1, 0, 0, 0);

    if (marker_bytes != 0) {
        vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, marker_pipeline);
        vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &marker_buffer.handle, &offset);
        vkCmdDraw(fd.command_buffer, uint32_t(markers.triangles.size()), 1, 0, 0);
    }
    if (line_bytes != 0) {
        vkCmdBindPipeline(fd.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, line_pipeline);
        vkCmdBindVertexBuffers(fd.command_buffer, 0, 1, &line_buffer.handle, &offset);
        vkCmdDraw(fd.command_buffer, uint32_t(markers.lines.size()), 1, 0, 0);
    }
    vkCmdEndRenderPass(fd.command_buffer);
    return check(vkEndCommandBuffer(fd.command_buffer), "End command buffer");
}
} // namespace graphics
