#pragma once

#include <DirectXMath.h>

#include <Scene/CmdListRecorder.h>

class BasicCmdListRecorder : public CmdListRecorder {
public:
	explicit BasicCmdListRecorder(ID3D12Device& device, tbb::concurrent_queue<ID3D12CommandList*>& cmdListQueue);

	__forceinline VertexAndIndexBufferDataVec& GetVertexAndIndexBufferDataVec() noexcept { return mVertexAndIndexBufferDataVec; }
	__forceinline MatricesVec& WorldMatrices() noexcept { return mWorldMatrices; }
	__forceinline UploadBuffer* &MaterialsCBuffer() noexcept { return mMaterialsCBuffer; }
	__forceinline D3D12_GPU_DESCRIPTOR_HANDLE& MaterialsCBufferGpuDescHandleBegin() noexcept { return mMaterialsCBufferGpuDescHandleBegin; }

	void RecordCommandLists(
		const DirectX::XMFLOAT4X4& view,
		const DirectX::XMFLOAT4X4& proj,
		const D3D12_CPU_DESCRIPTOR_HANDLE* rtvCpuDescHandles,
		const std::uint32_t rtvCpuDescHandlesCount,
		const D3D12_CPU_DESCRIPTOR_HANDLE& depthStencilHandle) noexcept override;	

	bool ValidateData() const noexcept;

private:
	// We should have a vector of world matrices per geometry.	
	VertexAndIndexBufferDataVec mVertexAndIndexBufferDataVec;
	MatricesVec mWorldMatrices;

	D3D12_GPU_DESCRIPTOR_HANDLE mMaterialsCBufferGpuDescHandleBegin;
	UploadBuffer* mMaterialsCBuffer{ nullptr };
};