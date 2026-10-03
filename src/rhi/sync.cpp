#include <ore/rhi/sync.h>

#include <ore/core/assert.h>

namespace ore::rhi {

Scope<Fence> Fence::create(Device& device, bool signaled) {
    Scope<Fence> fence(new Fence());
    fence->device_ = device.handle();
    VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (signaled) info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    ORE_VK_CHECK(vkCreateFence(fence->device_, &info, nullptr, &fence->fence_));
    return fence;
}

Fence::~Fence() {
    if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
}

bool Fence::wait(u64 timeout_ns) const {
    const VkResult result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, timeout_ns);
    if (result == VK_TIMEOUT) return false;
    ORE_VK_CHECK(result);
    return true;
}

void Fence::reset() { ORE_VK_CHECK(vkResetFences(device_, 1, &fence_)); }

bool Fence::signaled() const { return vkGetFenceStatus(device_, fence_) == VK_SUCCESS; }

Scope<Semaphore> Semaphore::create(Device& device) {
    Scope<Semaphore> semaphore(new Semaphore());
    semaphore->device_ = device.handle();
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    ORE_VK_CHECK(vkCreateSemaphore(semaphore->device_, &info, nullptr, &semaphore->semaphore_));
    return semaphore;
}

Semaphore::~Semaphore() {
    if (semaphore_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore_, nullptr);
}

Scope<TimelineSemaphore> TimelineSemaphore::create(Device& device, u64 initial_value) {
    Scope<TimelineSemaphore> semaphore(new TimelineSemaphore());
    semaphore->device_ = device.handle();
    VkSemaphoreTypeCreateInfo type_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type_info.initialValue = initial_value;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = &type_info;
    ORE_VK_CHECK(vkCreateSemaphore(semaphore->device_, &info, nullptr, &semaphore->semaphore_));
    return semaphore;
}

TimelineSemaphore::~TimelineSemaphore() {
    if (semaphore_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore_, nullptr);
}

u64 TimelineSemaphore::value() const {
    u64 value = 0;
    ORE_VK_CHECK(vkGetSemaphoreCounterValue(device_, semaphore_, &value));
    return value;
}

bool TimelineSemaphore::wait(u64 value, u64 timeout_ns) const {
    VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    info.semaphoreCount = 1;
    info.pSemaphores = &semaphore_;
    info.pValues = &value;
    const VkResult result = vkWaitSemaphores(device_, &info, timeout_ns);
    if (result == VK_TIMEOUT) return false;
    ORE_VK_CHECK(result);
    return true;
}

void TimelineSemaphore::signal_host(u64 value) const {
    VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    info.semaphore = semaphore_;
    info.value = value;
    ORE_VK_CHECK(vkSignalSemaphore(device_, &info));
}

void TimelineSemaphore::wait_host(u64 value) const { ORE_VK_CHECK(wait(value) ? VK_SUCCESS : VK_TIMEOUT); }

} // namespace ore::rhi
