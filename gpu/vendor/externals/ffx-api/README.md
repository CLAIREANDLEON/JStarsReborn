# AMD FSR SDK 2.4.0 API headers (C API only)

Copied unchanged from https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK at commit
`7539ae8b6568ad29ed78a753404351daf3b5cca7` (tag v2.4.0), `Kits/FidelityFX/{api,upscalers,framegeneration}/include` (DX12 headers left out), in the same layout.
These headers are MIT-licensed (notice at the top of each file). Only the headers are vendored: the signed effect DLLs
(`amd_fidelityfx_loader_vk.dll`, `..._upscaler_vk.dll`, `..._framegeneration_vk.dll`) are AMD binaries under AMD's own
redistribution terms and are **not** part of this repository; the runtime loads them at run time only if they are present.
