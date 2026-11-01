// JStarsReborn: Tracy's Vulkan context is not built; the types exist so declarations compile.
#pragma once
#include "Tracy.hpp"
namespace tracy { struct VkCtxScope; struct VkCtx; }
using TracyVkCtx = tracy::VkCtx*;
#define TracyVkCollect(ctx, cmdbuf) ((void)(ctx), (void)(cmdbuf))
#define TracyVkNamedZoneC(ctx, varname, cmdbuf, name, color, active) ((void)(ctx), (void)(cmdbuf))
#define TracyVkContextHostCalibrated(...) (static_cast<TracyVkCtx>(nullptr))
#define TracyVkContextName(ctx, name, size) ((void)(ctx), (void)(name), (void)(size))
#define TracyVkDestroy(ctx) ((void)(ctx))
