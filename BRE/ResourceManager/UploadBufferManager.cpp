#include "UploadBufferManager.h"

#include <memory>

#include <DirectXManager/DirectXManager.h>
#include <Utils/DebugUtils.h>
#include <Utils/NumberGeneration.h>

UploadBufferManager::UploadBufferById UploadBufferManager::mUploadBufferById;
std::mutex UploadBufferManager::mMutex;

std::size_t UploadBufferManager::CreateUploadBuffer(
	const std::size_t elementSize,
	const std::uint32_t elementCount,
	UploadBuffer*& uploadBuffer) noexcept
{
	const std::size_t id{ NumberGeneration::GetIncrementalSizeT() };
	UploadBufferById::accessor accessor;
#ifdef _DEBUG
	mUploadBufferById.find(accessor, id);
	ASSERT(accessor.empty());
#endif
	mUploadBufferById.insert(accessor, id);
	accessor->second = std::make_unique<UploadBuffer>(DirectXManager::GetDevice(), elementSize, elementCount);
	uploadBuffer = accessor->second.get();
	accessor.release();

	return id;
}

UploadBuffer& UploadBufferManager::GetUploadBuffer(const size_t id) noexcept {
	UploadBufferById::accessor accessor;
	mUploadBufferById.find(accessor, id);
	ASSERT(!accessor.empty());
	UploadBuffer* elem{ accessor->second.get() };
	accessor.release();

	return *elem;
}
