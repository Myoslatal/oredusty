// Ore framework - umbrella header.
//
// Include this to pull in the whole public API, or include the individual headers
// (<ore/renderer/renderer.h>, <ore/app/application.h>, ...) for faster builds.
#pragma once

// core
#include <ore/config.h>
#include <ore/core/assert.h>
#include <ore/core/block_allocator.h>
#include <ore/core/cli.h>
#include <ore/core/file.h>
#include <ore/core/image.h>
#include <ore/core/log.h>
#include <ore/core/time.h>
#include <ore/core/types.h>

// math
#include <ore/math/camera.h>
#include <ore/math/math.h>

// platform
#include <ore/platform/input.h>
#include <ore/platform/window.h>

// render hardware interface
#include <ore/rhi/allocator.h>
#include <ore/rhi/buffer.h>
#include <ore/rhi/commands.h>
#include <ore/rhi/context.h>
#include <ore/rhi/descriptor.h>
#include <ore/rhi/device.h>
#include <ore/rhi/instance.h>
#include <ore/rhi/pipeline.h>
#include <ore/rhi/render_target.h>
#include <ore/rhi/shader.h>
#include <ore/rhi/shader_compiler.h>
#include <ore/rhi/swapchain.h>
#include <ore/rhi/sync.h>
#include <ore/rhi/texture.h>
#include <ore/rhi/vulkan.h>

// renderer front end
#include <ore/renderer/mesh.h>
#include <ore/renderer/renderer.h>
#include <ore/renderer/scene.h>
#include <ore/renderer/upload_ring.h>

// application
#include <ore/app/application.h>
