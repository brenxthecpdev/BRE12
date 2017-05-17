#include "FenceManager.h"

#include <DirectXManager/DirectXManager.h>
#include <Utils/DebugUtils.h>

namespace BRE {
tbb::concurrent_unordered_set<ID3D12Fence*> FenceManager::mFences;
std::mutex FenceManager::mMutex;

void
FenceManager::Clear() noexcept
{
    for (ID3D12Fence* fence : mFences) {
        BRE_ASSERT(fence != nullptr);
        fence->Release();
    }

    mFences.clear();
}

ID3D12Fence&
FenceManager::CreateFence(const std::uint64_t fenceInitialValue,
                          const D3D12_FENCE_FLAGS& flags) noexcept
{
    ID3D12Fence* fence{ nullptr };

    mMutex.lock();
    BRE_CHECK_HR(DirectXManager::GetDevice().CreateFence(fenceInitialValue,
                                                         flags,
                                                         IID_PPV_ARGS(&fence)));
    mMutex.unlock();

    BRE_ASSERT(fence != nullptr);
    mFences.insert(fence);

    return *fence;
}
}