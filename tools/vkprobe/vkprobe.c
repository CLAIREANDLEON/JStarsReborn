/* vkprobe: read-only Vulkan capability probe for the two renderer findings of checkpoint 0.3.0.
 *   1. "image format Bc5UnormBlock type 2D is not supported (flags MutableFormat|BlockTexelViewCompatible|ExtendedUsage,
 *      usage TransferSrc|TransferDst|Sampled|Storage)" (texture_cache/image.cpp)
 *   2. VUID-VkBufferCreateInfo-size-06409: a 4 GiB buffer against maxBufferSize (buffer_cache arenas)
 * It creates an instance and queries the first discrete (else first) physical device; it creates no device, no
 * buffers and no images. Output: plain text on stdout. Build: x86_64-w64-mingw32-gcc -O2 vkprobe.c -o vkprobe.exe
 * (the Vulkan loader vulkan-1.dll is loaded at run time). GPL-2.0-or-later, part of JStarsReborn. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define LOADLIB() LoadLibraryA("vulkan-1.dll")
#define GETSYM(l, n) ((void*)GetProcAddress((HMODULE)(l), n))
#else
#include <dlfcn.h>
#define LOADLIB() dlopen("libvulkan.so.1", RTLD_NOW)
#define GETSYM(l, n) dlsym(l, n)
#endif

static PFN_vkGetInstanceProcAddr gipa;
#define IFN(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name)

static const char* res(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "supported";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "NOT SUPPORTED";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
    default: return "error";
    }
}

int main(void) {
    void* lib = LOADLIB();
    if (!lib) { printf("vulkan loader not found\n"); return 2; }
    gipa = (PFN_vkGetInstanceProcAddr)GETSYM(lib, "vkGetInstanceProcAddr");
    IFN(NULL, vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkprobe", 1, "JStarsReborn", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("vkCreateInstance failed\n"); return 3; }
    IFN(inst, vkEnumeratePhysicalDevices);
    IFN(inst, vkGetPhysicalDeviceProperties2);
    IFN(inst, vkGetPhysicalDeviceFormatProperties);
    IFN(inst, vkGetPhysicalDeviceImageFormatProperties2);
    IFN(inst, vkGetPhysicalDeviceFeatures);
    uint32_t n = 8; VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        vkGetPhysicalDeviceProperties2(pds[i], &p);
        printf("device %u: %s (type %d, api %u.%u.%u, driver 0x%x)\n", i, p.properties.deviceName, p.properties.deviceType,
               VK_API_VERSION_MAJOR(p.properties.apiVersion), VK_API_VERSION_MINOR(p.properties.apiVersion),
               VK_API_VERSION_PATCH(p.properties.apiVersion), p.properties.driverVersion);
        if (pd == VK_NULL_HANDLE || p.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            if (pd == VK_NULL_HANDLE || p.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pd = pds[i];
        }
    }
    if (!pd) { printf("no device\n"); return 4; }
    VkPhysicalDeviceMaintenance4Properties m4 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
    VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &m4};
    VkPhysicalDeviceProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    vkGetPhysicalDeviceProperties2(pd, &p);
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(pd, &f);
    printf("\nprobed: %s | driver %s %s\n", p.properties.deviceName, drv.driverName, drv.driverInfo);
    printf("maxBufferSize (maintenance4) = %llu bytes (%.2f GiB)\n", (unsigned long long)m4.maxBufferSize, m4.maxBufferSize / 1073741824.0);
    printf("sparseAddressSpaceSize = %llu bytes; sparseBinding=%u sparseResidencyBuffer=%u\n",
           (unsigned long long)p.properties.limits.sparseAddressSpaceSize, f.sparseBinding, f.sparseResidencyBuffer);
    printf("maxStorageBufferRange = %u\n", p.properties.limits.maxStorageBufferRange);

    struct { VkFormat f; const char* n; } fmts[] = {
        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1_RGBA_UNORM"}, {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1_RGBA_SRGB"},
        {VK_FORMAT_BC2_UNORM_BLOCK, "BC2_UNORM"}, {VK_FORMAT_BC3_UNORM_BLOCK, "BC3_UNORM"}, {VK_FORMAT_BC3_SRGB_BLOCK, "BC3_SRGB"},
        {VK_FORMAT_BC4_UNORM_BLOCK, "BC4_UNORM"}, {VK_FORMAT_BC4_SNORM_BLOCK, "BC4_SNORM"},
        {VK_FORMAT_BC5_UNORM_BLOCK, "BC5_UNORM"}, {VK_FORMAT_BC5_SNORM_BLOCK, "BC5_SNORM"},
        {VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H_UFLOAT"}, {VK_FORMAT_BC7_UNORM_BLOCK, "BC7_UNORM"}, {VK_FORMAT_BC7_SRGB_BLOCK, "BC7_SRGB"},
        {VK_FORMAT_R32G32_UINT, "R32G32_UINT (64-bit block class)"}, {VK_FORMAT_R32G32B32A32_UINT, "R32G32B32A32_UINT (128-bit block class)"},
        {VK_FORMAT_R16G16B16A16_UINT, "R16G16B16A16_UINT"}, {VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT"},
    };
    printf("\n%-40s %-9s %-9s | image query, flags Mutable|Extended|BlockTexelView, usage TS|TD|Sampled:  +Storage / without Storage | without BlockTexelView: +Storage\n",
           "format", "sampled", "storage");
    for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, fmts[i].f, &fp);
        int is_block = fmts[i].f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && fmts[i].f <= VK_FORMAT_BC7_SRGB_BLOCK;
        VkImageUsageFlags base = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkImageCreateFlags fl = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | (is_block ? VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT : 0);
        VkResult r[3];
        for (int k = 0; k < 3; k++) {
            VkPhysicalDeviceImageFormatInfo2 fi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, NULL, fmts[i].f, VK_IMAGE_TYPE_2D,
                                                   VK_IMAGE_TILING_OPTIMAL, base | (k != 1 ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
                                                   k == 2 ? (fl & ~VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) : fl};
            VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
            r[k] = vkGetPhysicalDeviceImageFormatProperties2(pd, &fi, &ip);
        }
        printf("%-40s %-9s %-9s | %-13s / %-13s | %s\n", fmts[i].n,
               fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? "yes" : "no",
               fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ? "yes" : "no", res(r[0]), res(r[1]), res(r[2]));
    }
    return 0;
}
