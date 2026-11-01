// dispatch-repro: replays ONE compute dispatch dumped by the renderer (NXRT_DUMP_DISPATCH / NXRT_DUMP_DISPATCH_DIR)
// on its own, so a GPU hang can be bisected outside the game by editing the SPIR-V.
//
//   dispatch-repro <dump dir> <index> <shader.spv> [--robust=0|1] [--mem=host|device] [--gpu=N] [--timeout-ms=N]
//                   [--out=DIR] [--iters=N]
// --out writes every buffer's final contents (<kind>-b<binding>-k<k>.bin) so two SPIR-V variants can be compared
// byte for byte; --iters repeats the dispatch N times in one submit (timing; output only meaningful with 1).
//
// Memory layout mimics the renderer: every buffer is a range of one 64 MiB stream buffer in the first memory type
// that is HOST_VISIBLE|HOST_COHERENT (--mem=host), or copied into DEVICE_LOCAL memory (--mem=device).
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "%s failed: %d\n", #x, r_); std::exit(2); } } while (0)

static std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); std::exit(2); }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Res { bool ubo; uint32_t binding, k; std::vector<uint8_t> data; VkDeviceSize offset = 0; };

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s <dump dir> <index> <shader.spv> [options]\n", argv[0]); return 1; }
    const std::string dir = argv[1], index = argv[2], spv_path = argv[3];
    int robust = 1, gpu = -1; bool device_mem = false; uint64_t timeout_ms = 10000; std::string out_dir; uint32_t iters = 1;
    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--robust=", 0) == 0) robust = std::atoi(a.c_str() + 9);
        else if (a.rfind("--mem=", 0) == 0) device_mem = a.substr(6) == "device";
        else if (a.rfind("--gpu=", 0) == 0) gpu = std::atoi(a.c_str() + 6);
        else if (a.rfind("--out=", 0) == 0) out_dir = a.substr(6);
        else if (a.rfind("--iters=", 0) == 0) iters = (uint32_t)std::strtoul(a.c_str() + 8, nullptr, 10);
        else if (a.rfind("--timeout-ms=", 0) == 0) timeout_ms = std::strtoull(a.c_str() + 13, nullptr, 10);
    }
    // ---- meta
    std::vector<Res> res; uint32_t groups[3] = {1, 1, 1};
    {
        std::ifstream m(dir + "/d" + index + "-meta.txt");
        if (!m) { std::fprintf(stderr, "no meta in %s\n", dir.c_str()); return 2; }
        std::string line;
        while (std::getline(m, line)) {
            std::istringstream in(line); std::string kind; in >> kind;
            if (kind == "ubo" || kind == "ssbo") {
                Res r; uint32_t b, k; size_t size; in >> b >> k >> size;
                r.ubo = kind == "ubo"; r.binding = b; r.k = k;
                r.data = ReadFile(dir + "/d" + index + "-" + kind + "-b" + std::to_string(b) + "-k" + std::to_string(k) + ".bin");
                res.push_back(std::move(r));
            } else if (kind == "groups") in >> groups[0] >> groups[1] >> groups[2];
        }
    }
    const std::vector<uint8_t> spv = ReadFile(spv_path);

    // ---- instance / device
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VkInstance inst; CHECK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(inst, &n, devs.data());
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(devs[i], &p);
        std::printf("gpu %u: %s\n", i, p.deviceName);
        if (gpu >= 0 ? (int)i == gpu : (phys == VK_NULL_HANDLE && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) phys = devs[i];
    }
    if (!phys) phys = devs[0];
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys, &props);
    std::printf("usando: %s | robustBufferAccess=%d memoria=%s\n", props.deviceName, robust, device_mem ? "device-local" : "host-visible");

    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.shaderInt8 = f12.shaderInt8; e12.storageBuffer8BitAccess = f12.storageBuffer8BitAccess;
    e12.uniformAndStorageBuffer8BitAccess = f12.uniformAndStorageBuffer8BitAccess; e12.shaderFloat16 = f12.shaderFloat16;
    VkPhysicalDeviceVulkan11Features e11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &e12};
    e11.storageBuffer16BitAccess = f11.storageBuffer16BitAccess;
    e11.uniformAndStorageBuffer16BitAccess = f11.uniformAndStorageBuffer16BitAccess;
    e11.variablePointers = f11.variablePointers; e11.variablePointersStorageBuffer = f11.variablePointersStorageBuffer;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e11};
    e2.features.robustBufferAccess = robust ? f2.features.robustBufferAccess : VK_FALSE;
    e2.features.shaderInt16 = f2.features.shaderInt16; e2.features.shaderInt64 = f2.features.shaderInt64;
    e2.features.shaderFloat64 = f2.features.shaderFloat64;
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf.data());
    uint32_t qfi = 0; for (uint32_t i = 0; i < qn; ++i) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfi = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, qfi, 1, &prio};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &e2, 0, 1, &qci};
    VkDevice dev; CHECK(vkCreateDevice(phys, &dci, nullptr, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, qfi, 0, &queue);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    auto find_mem = [&](uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        std::fprintf(stderr, "no memory type\n"); std::exit(2); };

    // ---- buffers: one stream buffer, ranges at 256-byte aligned offsets
    VkDeviceSize total = 0;
    for (Res& r : res) { r.offset = total; total += (std::max<size_t>(r.data.size(), 16) + 255) & ~size_t{255}; }
    const VkDeviceSize stream_size = std::max<VkDeviceSize>(total, 4096);
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto make_buffer = [&](VkDeviceSize size, VkMemoryPropertyFlags flags, VkBuffer& buf, VkDeviceMemory& mem) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE};
        CHECK(vkCreateBuffer(dev, &bci, nullptr, &buf));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev, buf, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, find_mem(req.memoryTypeBits, flags)};
        CHECK(vkAllocateMemory(dev, &mai, nullptr, &mem)); CHECK(vkBindBufferMemory(dev, buf, mem, 0));
        std::printf("buffer %llu bytes em memoryType %u (flags 0x%x)\n", (unsigned long long)size, mai.memoryTypeIndex,
                    mp.memoryTypes[mai.memoryTypeIndex].propertyFlags); };
    VkBuffer stream, upload = VK_NULL_HANDLE; VkDeviceMemory stream_mem, upload_mem = VK_NULL_HANDLE;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (device_mem) { make_buffer(stream_size, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, stream, stream_mem);
                      make_buffer(stream_size, host, upload, upload_mem); }
    else make_buffer(stream_size, host, stream, stream_mem);
    {
        void* map = nullptr; CHECK(vkMapMemory(dev, device_mem ? upload_mem : stream_mem, 0, stream_size, 0, &map));
        std::memset(map, 0, stream_size);
        for (Res& r : res) std::memcpy(static_cast<uint8_t*>(map) + r.offset, r.data.data(), r.data.size());
        vkUnmapMemory(dev, device_mem ? upload_mem : stream_mem);
    }

    // ---- pipeline
    std::map<uint32_t, std::pair<bool, uint32_t>> layout_bindings;  // binding -> (ubo, count)
    for (const Res& r : res) { auto& e = layout_bindings[r.binding]; e.first = r.ubo; e.second = std::max(e.second, r.k + 1); }
    std::vector<VkDescriptorSetLayoutBinding> lb;
    for (auto& [b, e] : layout_bindings)
        lb.push_back({b, e.first ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, e.second, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, (uint32_t)lb.size(), lb.data()};
    VkDescriptorSetLayout sl; CHECK(vkCreateDescriptorSetLayout(dev, &slci, nullptr, &sl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &sl, 1, &pcr};
    VkPipelineLayout pl; CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, spv.size(), reinterpret_cast<const uint32_t*>(spv.data())};
    VkShaderModule sm; CHECK(vkCreateShaderModule(dev, &smi, nullptr, &sm));
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr}, pl};
    VkPipeline pipe; CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));

    // ---- descriptors
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 2, ps};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &pool));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1, &sl};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(dev, &dai, &set));
    std::vector<VkDescriptorBufferInfo> infos(res.size()); std::vector<VkWriteDescriptorSet> writes;
    for (size_t i = 0; i < res.size(); ++i) {
        infos[i] = {stream, res[i].offset, std::max<size_t>(res[i].data.size(), 16)};
        writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, res[i].binding, res[i].k, 1,
                          res[i].ubo ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[i], nullptr});
    }
    vkUpdateDescriptorSets(dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);

    // ---- run
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, qfi};
    VkCommandPool cp; CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &cp));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    if (device_mem) { VkBufferCopy bc{0, 0, stream_size}; vkCmdCopyBuffer(cmd, upload, stream, 1, &bc);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr); }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
    uint8_t push[32] = {}; const float one = 1.0f; std::memcpy(push + 24, &one, 4);
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, push);
    for (uint32_t it = 0; it < std::max<uint32_t>(iters, 1); ++it) {
        if (it) { VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr); }
        vkCmdDispatch(cmd, groups[0], groups[1], groups[2]);
    }
    if (device_mem) { VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        VkBufferCopy bc{0, 0, stream_size}; vkCmdCopyBuffer(cmd, stream, upload, 1, &bc);
        VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr); }
    CHECK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CHECK(vkCreateFence(dev, &fci, nullptr, &fence));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd};
    std::printf("dispatch %u x %u x %u ...\n", groups[0], groups[1], groups[2]); std::fflush(stdout);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    const VkResult w = vkWaitForFences(dev, 1, &fence, VK_TRUE, timeout_ms * 1000000ull);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("RESULTADO: %s (VkResult %d) em %.1f ms\n", w == VK_SUCCESS ? "OK" : w == VK_TIMEOUT ? "TIMEOUT (travou)" : w == VK_ERROR_DEVICE_LOST ? "DEVICE_LOST" : "ERRO", w, ms);
    if (w == VK_SUCCESS) std::printf("iters=%u: %.3f ms/iter (inclui submit e espera)\n", std::max<uint32_t>(iters, 1), ms / std::max<uint32_t>(iters, 1));
    if (w == VK_SUCCESS && !out_dir.empty()) {
        void* map = nullptr; CHECK(vkMapMemory(dev, device_mem ? upload_mem : stream_mem, 0, stream_size, 0, &map));
        for (const Res& r : res) {
            std::ofstream f(out_dir + "/" + (r.ubo ? "ubo" : "ssbo") + "-b" + std::to_string(r.binding) + "-k" + std::to_string(r.k) + ".bin", std::ios::binary | std::ios::trunc);
            f.write(static_cast<const char*>(map) + r.offset, (std::streamsize)r.data.size());
        }
        vkUnmapMemory(dev, device_mem ? upload_mem : stream_mem);
    }
    return w == VK_SUCCESS ? 0 : 3;
}
