#include "EnvironmentLightCmdListRecorder.h"

#include <DirectXMath.h>

#include <CommandListExecutor\CommandListExecutor.h>
#include <CommandManager/CommandAllocatorManager.h>
#include <CommandManager/CommandListManager.h>
#include <DescriptorManager\CbvSrvUavDescriptorManager.h>
#include <PSOManager/PSOManager.h>
#include <ResourceManager/UploadBufferManager.h>
#include <RootSignatureManager\RootSignatureManager.h>
#include <ShaderManager\ShaderManager.h>
#include <ShaderUtils\CBuffers.h>
#include <Utils/DebugUtils.h>

// Root Signature:
// "CBV(b0, visibility = SHADER_VISIBILITY_VERTEX), " \ 0 -> Frame CBuffer
// "CBV(b0, visibility = SHADER_VISIBILITY_PIXEL), " \ 1 -> Frame CBuffer
// "DescriptorTable(SRV(t0), SRV(t1), SRV(t2), SRV(t3), SRV(t4), visibility = SHADER_VISIBILITY_PIXEL), " \ 2 -> Textures 

namespace {
	ID3D12PipelineState* sPSO{ nullptr };
	ID3D12RootSignature* sRootSignature{ nullptr };

	void BuildCommandObjects(
		ID3D12GraphicsCommandList* &commandList, 
		ID3D12CommandAllocator* commandAllocators[], 
		const std::size_t commandAllocatorCount) noexcept {
		ASSERT(commandList == nullptr);

#ifdef _DEBUG
		for (std::uint32_t i = 0U; i < commandAllocatorCount; ++i) {
			ASSERT(commandAllocators[i] == nullptr);
		}
#endif

		for (std::uint32_t i = 0U; i < commandAllocatorCount; ++i) {
			commandAllocators[i] = &CommandAllocatorManager::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT);
		}

		commandList = &CommandListManager::CreateCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT, *commandAllocators[0]);

		// Start off in a closed state.  This is because the first time we refer 
		// to the command list we will Reset it, and it needs to be closed before
		// calling Reset.
		commandList->Close();
	}
}

EnvironmentLightCmdListRecorder::EnvironmentLightCmdListRecorder() {
	BuildCommandObjects(mCommandList, mCommandAllocators, _countof(mCommandAllocators));
}

void EnvironmentLightCmdListRecorder::InitPSO() noexcept {
	ASSERT(sPSO == nullptr);
	ASSERT(sRootSignature == nullptr);

	// Build pso and root signature
	PSOManager::PSOCreationData psoData{};
	const std::size_t renderTargetCount{ _countof(psoData.mRenderTargetFormats) };
	psoData.mBlendDescriptor = D3DFactory::GetAlwaysBlendDesc();
	psoData.mDepthStencilDescriptor = D3DFactory::GetDisabledDepthStencilDesc();

	psoData.mPixelShaderBytecode = ShaderManager::LoadShaderFileAndGetBytecode("EnvironmentLightPass/Shaders/PS.cso");
	psoData.mVertexShaderBytecode = ShaderManager::LoadShaderFileAndGetBytecode("EnvironmentLightPass/Shaders/VS.cso");

	ID3DBlob* rootSignatureBlob = &ShaderManager::LoadShaderFileAndGetBlob("EnvironmentLightPass/Shaders/RS.cso");
	psoData.mRootSignature = &RootSignatureManager::CreateRootSignatureFromBlob(*rootSignatureBlob);
	sRootSignature = psoData.mRootSignature;

	psoData.mNumRenderTargets = 1U;
	psoData.mRenderTargetFormats[0U] = SettingsManager::sColorBufferFormat;
	for (std::size_t i = psoData.mNumRenderTargets; i < renderTargetCount; ++i) {
		psoData.mRenderTargetFormats[i] = DXGI_FORMAT_UNKNOWN;
	}
	psoData.mPrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	sPSO = &PSOManager::CreateGraphicsPSO(psoData);

	ASSERT(sPSO != nullptr);
	ASSERT(sRootSignature != nullptr);
}

void EnvironmentLightCmdListRecorder::Init(
	Microsoft::WRL::ComPtr<ID3D12Resource>* geometryBuffers,
	const std::uint32_t geometryBuffersCount,
	ID3D12Resource& depthBuffer,
	const D3D12_CPU_DESCRIPTOR_HANDLE& outputColorBufferCpuDesc,
	ID3D12Resource& diffuseIrradianceCubeMap,
	ID3D12Resource& specularPreConvolvedCubeMap) noexcept
{
	ASSERT(ValidateData() == false);
	ASSERT(geometryBuffers != nullptr);
	ASSERT(geometryBuffersCount > 0U);

	mOutputColorBufferCpuDesc = outputColorBufferCpuDesc;

	InitConstantBuffers();
	InitShaderResourceViews(geometryBuffers, geometryBuffersCount, depthBuffer, diffuseIrradianceCubeMap, specularPreConvolvedCubeMap);

	ASSERT(ValidateData());
}

void EnvironmentLightCmdListRecorder::RecordAndPushCommandLists(const FrameCBuffer& frameCBuffer) noexcept {
	ASSERT(ValidateData());
	ASSERT(sPSO != nullptr);
	ASSERT(sRootSignature != nullptr);

	static std::uint32_t currentFrameIndex = 0U;

	ID3D12CommandAllocator* commandAllocator{ mCommandAllocators[currentFrameIndex] };
	ASSERT(commandAllocator != nullptr);

	// Update frame constants
	UploadBuffer& uploadFrameCBuffer(*mFrameCBuffer[currentFrameIndex]);
	uploadFrameCBuffer.CopyData(0U, &frameCBuffer, sizeof(frameCBuffer));
	
	CHECK_HR(commandAllocator->Reset());
	CHECK_HR(mCommandList->Reset(commandAllocator, sPSO));

	mCommandList->RSSetViewports(1U, &SettingsManager::sScreenViewport);
	mCommandList->RSSetScissorRects(1U, &SettingsManager::sScissorRect);
	mCommandList->OMSetRenderTargets(1U, &mOutputColorBufferCpuDesc, false, nullptr);

	ID3D12DescriptorHeap* heaps[] = { &CbvSrvUavDescriptorManager::GetDescriptorHeap() };
	mCommandList->SetDescriptorHeaps(_countof(heaps), heaps);
	mCommandList->SetGraphicsRootSignature(sRootSignature);
	
	// Set root parameters
	const D3D12_GPU_VIRTUAL_ADDRESS frameCBufferGpuVAddress(uploadFrameCBuffer.GetResource()->GetGPUVirtualAddress());
	mCommandList->SetGraphicsRootConstantBufferView(0U, frameCBufferGpuVAddress);
	mCommandList->SetGraphicsRootConstantBufferView(1U, frameCBufferGpuVAddress);
	mCommandList->SetGraphicsRootDescriptorTable(2U, mPixelShaderBuffersGpuDesc);

	// Draw object
	mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	mCommandList->DrawInstanced(6U, 1U, 0U, 0U);

	mCommandList->Close();

	CommandListExecutor::Get().AddCommandList(*mCommandList);

	// Next frame
	currentFrameIndex = (currentFrameIndex + 1) % SettingsManager::sQueuedFrameCount;
}

bool EnvironmentLightCmdListRecorder::ValidateData() const noexcept {
	for (std::uint32_t i = 0UL; i < SettingsManager::sQueuedFrameCount; ++i) {
		if (mCommandAllocators[i] == nullptr) {
			return false;
		}
	}

	for (std::uint32_t i = 0UL; i < SettingsManager::sQueuedFrameCount; ++i) {
		if (mFrameCBuffer[i] == nullptr) {
			return false;
		}
	}

	const bool result =
		mCommandList != nullptr &&
		mOutputColorBufferCpuDesc.ptr != 0UL &&
		mPixelShaderBuffersGpuDesc.ptr != 0UL;

	return result;
}

void EnvironmentLightCmdListRecorder::InitShaderResourceViews(
	Microsoft::WRL::ComPtr<ID3D12Resource>* geometryBuffers, 
	const std::uint32_t geometryBuffersCount,
	ID3D12Resource& depthBuffer,
	ID3D12Resource& diffuseIrradianceCubeMap,
	ID3D12Resource& specularPreConvolvedCubeMap) noexcept
{
	ASSERT(geometryBuffers != nullptr);
	ASSERT(geometryBuffersCount > 0U);

	// Used to create SRV descriptors
	std::vector<D3D12_SHADER_RESOURCE_VIEW_DESC> srvDescriptors;
	srvDescriptors.resize(geometryBuffersCount + 3U); // 3 = depth buffer + 2 cube maps
	std::vector<ID3D12Resource*> res;
	res.resize(geometryBuffersCount + 3U);

	// Fill geometry buffers SRV descriptors
	for (std::uint32_t i = 0U; i < geometryBuffersCount; ++i) {
		ASSERT(geometryBuffers[i].Get() != nullptr);
		res[i] = geometryBuffers[i].Get();
		srvDescriptors[i].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDescriptors[i].ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDescriptors[i].Texture2D.MostDetailedMip = 0;
		srvDescriptors[i].Texture2D.ResourceMinLODClamp = 0.0f;
		srvDescriptors[i].Format = res[i]->GetDesc().Format;
		srvDescriptors[i].Texture2D.MipLevels = res[i]->GetDesc().MipLevels;
	}

	// Fill depth buffer descriptor
	std::uint32_t descIndex = geometryBuffersCount;
	srvDescriptors[descIndex].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDescriptors[descIndex].ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDescriptors[descIndex].Texture2D.MostDetailedMip = 0;
	srvDescriptors[descIndex].Texture2D.ResourceMinLODClamp = 0.0f;
	srvDescriptors[descIndex].Format = SettingsManager::sDepthStencilSRVFormat;
	srvDescriptors[descIndex].Texture2D.MipLevels = depthBuffer.GetDesc().MipLevels;
	res[descIndex] = &depthBuffer;
	++descIndex;

	// Fill cube map texture descriptors	
	srvDescriptors[descIndex].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDescriptors[descIndex].ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	srvDescriptors[descIndex].TextureCube.MostDetailedMip = 0;
	srvDescriptors[descIndex].TextureCube.MipLevels = diffuseIrradianceCubeMap.GetDesc().MipLevels;
	srvDescriptors[descIndex].TextureCube.ResourceMinLODClamp = 0.0f;
	srvDescriptors[descIndex].Format = diffuseIrradianceCubeMap.GetDesc().Format;
	res[descIndex] = &diffuseIrradianceCubeMap;
	++descIndex;

	srvDescriptors[descIndex].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDescriptors[descIndex].ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	srvDescriptors[descIndex].TextureCube.MostDetailedMip = 0;
	srvDescriptors[descIndex].TextureCube.MipLevels = specularPreConvolvedCubeMap.GetDesc().MipLevels;
	srvDescriptors[descIndex].TextureCube.ResourceMinLODClamp = 0.0f;
	srvDescriptors[descIndex].Format = specularPreConvolvedCubeMap.GetDesc().Format;
	res[descIndex] = &specularPreConvolvedCubeMap;
	++descIndex;

	mPixelShaderBuffersGpuDesc = CbvSrvUavDescriptorManager::CreateShaderResourceViews(res.data(), srvDescriptors.data(), static_cast<std::uint32_t>(srvDescriptors.size()));
}

void EnvironmentLightCmdListRecorder::InitConstantBuffers() noexcept {
#ifdef _DEBUG
	for (std::uint32_t i = 0U; i < SettingsManager::sQueuedFrameCount; ++i) {
		ASSERT(mFrameCBuffer[i] == nullptr);
	}
#endif

	// Create frame cbuffers
	const std::size_t frameCBufferElemSize{ UploadBuffer::GetRoundedConstantBufferSizeInBytes(sizeof(FrameCBuffer)) };
	for (std::uint32_t i = 0U; i < SettingsManager::sQueuedFrameCount; ++i) {
		mFrameCBuffer[i] = &UploadBufferManager::CreateUploadBuffer(frameCBufferElemSize, 1U);
	}
}