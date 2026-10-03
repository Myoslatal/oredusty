// Ore framework - fences and semaphores.
#pragma once

#include <ore/core/types.h>
#include <ore/rhi/device.h>

namespace ore::rhi {

class Fence {
public:
    [[nodiscard]] static Scope<Fence> create(Device& device, bool signaled = false);
    ~Fence();
    ORE_NON_MOVABLE(Fence);

    [[nodiscard]] VkFence handle() const { return fence_; }
    /// Waits for the fence to be signaled. Returns false on timeout.
    bool wait(u64 timeout_ns = ~0ull) const;
    void reset();
    [[nodiscard]] bool signaled() const;

private:
    Fence() = default;
    VkDevice device_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};

class Semaphore {
public:
    [[nodiscard]] static Scope<Semaphore> create(Device& device);
    ~Semaphore();
    ORE_NON_MOVABLE(Semaphore);

    [[nodiscard]] VkSemaphore handle() const { return semaphore_; }

private:
    Semaphore() = default;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
};

/// Vulkan 1.2 timeline semaphore: monotonic counter with host and device waits.
class TimelineSemaphore {
public:
    [[nodiscard]] static Scope<TimelineSemaphore> create(Device& device, u64 initial_value = 0);
    ~TimelineSemaphore();
    ORE_NON_MOVABLE(TimelineSemaphore);

    [[nodiscard]] VkSemaphore handle() const { return semaphore_; }
    [[nodiscard]] u64 value() const;
    /// Blocks the host until the semaphore reaches \p value. Returns false on timeout.
    bool wait(u64 value, u64 timeout_ns = ~0ull) const;
    void signal_host(u64 value) const;
    void wait_host(u64 value) const;

private:
    TimelineSemaphore() = default;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
};

} // namespace ore::rhi
