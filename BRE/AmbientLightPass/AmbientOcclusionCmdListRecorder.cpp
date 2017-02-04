#include "AmbientOcclusionCmdListRecorder.h"

#include <DirectXMath.h>

#include <CommandListExecutor\CommandListExecutor.h>
#include <CommandManager/CommandAllocatorManager.h>
#include <CommandManager/CommandListManager.h>
#include <DescriptorManager\CbvSrvUavDescriptorManager.h>
#include <DXUtils\d3dx12.h>
#include <MathUtils/MathUtils.h>
#include <PSOManager/PSOManager.h>
#include <ResourceManager/ResourceManager.h>
#include <ResourceManager/UploadBufferManager.h>
#include <ResourceStateManager\ResourceStateManager.h>
#include <RootSignatureManager\RootSignatureManager.h>
#include <ShaderManager\ShaderManager.h>
#include <ShaderUtils\CBuffers.h>
#include <Utils/DebugUtils.h>

using namespace DirectX;

// Root Signature:
// "CBV(b0, visibility = SHADER_VISIBILITY_VERTEX), " \ 0 -> Frame CBuffer
// "CBV(b0, visibility = SHADER_VISIBILITY_PIXEL), " \ 1 -> Frame CBuffer
// "DescriptorTable(SRV(t0), SRV(t1), SRV(t2), SRV(t3), visibility = SHADER_VISIBILITY_PIXEL)" 2 -> normal_smoothness + depth + sample kernel + kernel noise

namespace {
	ID3D12PipelineState* sPSO{ nullptr };
	ID3D12RootSignature* sRootSignature{ nullptr };

	void BuildCommandObjects(
		ID3D12GraphicsCommandList* &commandList, 
		ID3D12CommandAllocator* commandAllocators[], 
		const std::size_t commandAllocatorCount) noexcept 
	{
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

	// Sample kernel for ambient occlusion. The requirements are that:
	// - Sample positions fall within the unit hemisphere oriented
	//   toward positive z axis.
	// - Sample positions are more densely clustered towards the origin.
	//   This effectively attenuates the occlusion contribution
	//   according to distance from the sample kernel centre (samples closer
	//   to a point occlude it more than samples further away).
	void GenerateSampleKernel(const std::uint32_t sampleKernelSize, std::vector<XMFLOAT4>& sampleKernel) {
		ASSERT(sampleKernelSize > 0U);

		sampleKernel.reserve(sampleKernelSize);
		const float sampleKernelSizeFloat = static_cast<float>(sampleKernelSize);
		XMVECTOR vec;
		for (std::uint32_t i = 0U; i < sampleKernelSize; ++i) {
			const float x = MathUtils::RandomFloatInInverval(-1.0f, 1.0f);
			const float y = MathUtils::RandomFloatInInverval(-1.0f, 1.0f);
			const float z = MathUtils::RandomFloatInInverval(0.0f, 1.0f);
			sampleKernel.push_back(XMFLOAT4(x, y, z, 0.0f));
			XMFLOAT4& currentSample = sampleKernel.back();
			vec = XMLoadFloat4(&currentSample);
			vec = XMVector4Normalize(vec);
			XMStoreFloat4(&currentSample, vec);

			// Accelerating interpolation function to falloff 
			// from the distance from the origin.
			float scale = i / sampleKernelSizeFloat;
			scale = MathUtils::Lerp(0.1f, 1.0f, scale * scale);
			vec = XMVectorScale(vec, scale);
			XMStoreFloat4(&currentSample, vec);
		}
	}

	// Generate a set of random values used to rotate the sample kernel,
	// which will effectively increase the sample count and minimize 
	// the 'banding' artifacts.
	void GenerateNoise(const std::uint32_t numSamples, std::vector<XMFLOAT4>& noiseVectors) {
		ASSERT(numSamples > 0U);

		noiseVectors.reserve(numSamples);
		XMVECTOR vec;
		for (std::uint32_t i = 0U; i < numSamples; ++i) {
			const float x = MathUtils::RandomFloatInInverval(-1.0f, 1.0f);
			const float y = MathUtils::RandomFloatInInverval(-1.0f, 1.0f);
			// The z component must zero. Since our kernel is oriented along the z-axis, 
			// we want the random rotation to occur around that axis.
			const float z = 0.0f;
			noiseVectors.push_back(XMFLOAT4(x, y, z, 0.0f));
			XMFLOAT4& currentSample = noiseVectors.back();
			vec = XMLoadFloat4(&currentSample);
			vec = XMVector4Normalize(vec);
			XMStoreFloat4(&currentSample, vec);

			// Map from [-1.0f, 1.0f] to [0.0f, 1.0f] because
			// this is going to be stored in a texture
			currentSample.x = currentSample.x * 0.5f + 0.5f;
			currentSample.y = currentSample.y * 0.5f + 0.5f;
			currentSample.z = currentSample.z * 0.5f + 0.5f;
		}
	}
}

AmbientOcclusionCmdListRecorder::AmbientOcclusionCmdListRecorder() {
	BuildCommandObjects(mCommandList, mCommandAllocators, _countof(mCommandAllocators));
}

void AmbientOcclusionCmdListRecorder::InitPSO() noexcept {
	ASSERT(sPSO == nullptr);
	ASSERT(sRootSignature == nullptr);

	// Build pso and root signature
	PSOManager::PSOCreationData psoData{};
	const std::size_t renderTargetCount{ _countof(psoData.mRenderTargetFormats) };
	psoData.mBlendDescriptor = D3DFactory::GetAlwaysBlendDesc();
	psoData.mDepthStencilDescriptor = D3DFactory::GetDisabledDepthStencilDesc();

	psoData.mPixelShaderBytecode = ShaderManager::LoadShaderFileAndGetBytecode("AmbientLightPass/Shaders/AmbientOcclusion/PS.cso");
	psoData.mVertexShaderBytecode = ShaderManager::LoadShaderFileAndGetBytecode("AmbientLightPass/Shaders/AmbientOcclusion/VS.cso");

	ID3DBlob* rootSignatureBlob = &ShaderManager::LoadShaderFileAndGetBlob("AmbientLightPass/Shaders/AmbientOcclusion/RS.cso");
	psoData.mRootSignature = &RootSignatureManager::CreateRootSignatureFromBlob(*rootSignatureBlob);
	sRootSignature = psoData.mRootSignature;

	psoData.mNumRenderTargets = 1U;
	psoData.mRenderTargetFormats[0U] = DXGI_FORMAT_R16_UNORM;
	for (std::size_t i = psoData.mNumRenderTargets; i < renderTargetCount; ++i) {
		psoData.mRenderTargetFormats[i] = DXGI_FORMAT_UNKNOWN;
	}
	psoData.mPrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	sPSO = &PSOManager::CreateGraphicsPSO(psoData);

	ASSERT(sPSO != nullptr);
	ASSERT(sRootSignature != nullptr);
}

void AmbientOcclusionCmdListRecorder::Init(
	ID3D12Resource& normalSmoothnessBuffer,	
	const D3D12_CPU_DESCRIPTOR_HANDLE& ambientAccessBufferCpuDesc,
	ID3D12Resource& depthBuffer) noexcept
{
	ASSERT(ValidateData() == false);

	mAmbientAccessibilityBufferCpuDesc = ambientAccessBufferCpuDesc;

	mSampleKernelSize = 128U;
	mNoiseTextureDimension = 4U;
	std::vector<XMFLOAT4> sampleKernel;
	GenerateSampleKernel(mSampleKernelSize, sampleKernel);
	std::vector<XMFLOAT4> noises;
	GenerateNoise(mNoiseTextureDimension * mNoiseTextureDimension, noises);
	BuildBuffers(sampleKernel.data(), noises.data(), normalSmoothnessBuffer, depthBuffer);

	ASSERT(ValidateData());
}

void AmbientOcclusionCmdListRecorder::RecordAndPushCommandLists(const FrameCBuffer& frameCBuffer) noexcept {
	ASSERT(ValidateData());
	ASSERT(sPSO != nullptr);
	ASSERT(sRootSignature != nullptr);

	static std::uint32_t currentFrameIndex = 0U;

	ID3D12CommandAllocator* commandAllocator{ mCommandAllocators[currentFrameIndex] };
	ASSERT(commandAllocator != nullptr);
	
	CHECK_HR(commandAllocator->Reset());
	CHECK_HR(mCommandList->Reset(commandAllocator, sPSO));

	// Update frame constants
	UploadBuffer& uploadFrameCBuffer(*mFrameCBuffer[currentFrameIndex]);
	uploadFrameCBuffer.CopyData(0U, &frameCBuffer, sizeof(frameCBuffer));

	mCommandList->RSSetViewports(1U, &SettingsManager::sScreenViewport);
	mCommandList->RSSetScissorRects(1U, &SettingsManager::sScissorRect);
	mCommandList->OMSetRenderTargets(1U, &mAmbientAccessibilityBufferCpuDesc, false, nullptr);

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

bool AmbientOcclusionCmdListRecorder::ValidateData() const noexcept {
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
		mSampleKernelSize != 0U &&
		mSampleKernelBuffer != nullptr &&
		mSampleKernelBufferGpuDescBegin.ptr != 0UL &&
		mAmbientAccessibilityBufferCpuDesc.ptr != 0UL &&
		mPixelShaderBuffersGpuDesc.ptr != 0UL;

	return result;
}

void AmbientOcclusionCmdListRecorder::BuildBuffers(
	const void* randomSamples, 
	const void* noiseVectors,
	ID3D12Resource& normalSmoothnessBuffer,
	ID3D12Resource& depthBuffer) noexcept 
{
#ifdef _DEBUG
	for (std::uint32_t i = 0U; i < SettingsManager::sQueuedFrameCount; ++i) {
		ASSERT(mFrameCBuffer[i] == nullptr);
	}
#endif
	ASSERT(mSampleKernelBuffer == nullptr);
	ASSERT(randomSamples != nullptr);
	ASSERT(noiseVectors != nullptr);
	ASSERT(mSampleKernelSize != 0U);

	// Create sample kernel buffer and fill it
	const std::size_t sampleKernelBufferElemSize{ sizeof(XMFLOAT4) };
	mSampleKernelBuffer = &UploadBufferManager::CreateUploadBuffer(sampleKernelBufferElemSize, mSampleKernelSize);
	const std::uint8_t* sampleKernelPtr = reinterpret_cast<const std::uint8_t*>(randomSamples);
	for (std::uint32_t i = 0UL; i < mSampleKernelSize; ++i) {
		mSampleKernelBuffer->CopyData(i, sampleKernelPtr + sampleKernelBufferElemSize * i, sampleKernelBufferElemSize);
	}
	mSampleKernelBufferGpuDescBegin.ptr = mSampleKernelBuffer->GetResource()->GetGPUVirtualAddress();

	// Kernel noise resource and fill it
	D3D12_RESOURCE_DESC resDesc = {};
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resDesc.Alignment = 0U;
	resDesc.Width = mNoiseTextureDimension;
	resDesc.Height = mNoiseTextureDimension;
	resDesc.DepthOrArraySize = 1U;
	resDesc.MipLevels = 1U;
	resDesc.SampleDesc.Count = 1U;
	resDesc.SampleDesc.Quality = 0U;
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resDesc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
	resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	ID3D12Resource* noiseTexture{ nullptr };
	CD3DX12_HEAP_PROPERTIES heapProps{ D3D12_HEAP_TYPE_DEFAULT };
	noiseTexture = &ResourceManager::CreateCommittedResource(
		heapProps, 
		D3D12_HEAP_FLAG_NONE, 
		resDesc, 
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 
		nullptr,
		L"Noise Buffer");

	// In order to copy CPU memory data into our default buffer, we need to create
	// an intermediate upload heap. 
	const std::uint32_t num2DSubresources = resDesc.DepthOrArraySize * resDesc.MipLevels;
	const std::size_t uploadBufferSize = GetRequiredIntermediateSize(noiseTexture, 0, num2DSubresources);
	ID3D12Resource* noiseTextureUploadBuffer{ nullptr };
	noiseTextureUploadBuffer = &ResourceManager::CreateCommittedResource(
		CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		nullptr);

	D3D12_SUBRESOURCE_DATA subResourceData = {};
	subResourceData.pData = noiseVectors;
	subResourceData.RowPitch = mNoiseTextureDimension * sizeof(XMFLOAT4);
	subResourceData.SlicePitch = subResourceData.RowPitch * mNoiseTextureDimension;

	// Create frame cbuffers
	const std::size_t frameCBufferElemSize{ UploadBuffer::GetRoundedConstantBufferSizeInBytes(sizeof(FrameCBuffer)) };
	for (std::uint32_t i = 0U; i < SettingsManager::sQueuedFrameCount; ++i) {
		mFrameCBuffer[i] = &UploadBufferManager::CreateUploadBuffer(frameCBufferElemSize, 1U);
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc[4U]{};
	ID3D12Resource* res[4] = {
		&normalSmoothnessBuffer,
		&depthBuffer,
		mSampleKernelBuffer->GetResource(),
		noiseTexture,
	};

	// Fill normal_smoothness buffer texture descriptor
	srvDesc[0].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc[0].ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc[0].Texture2D.MostDetailedMip = 0;
	srvDesc[0].Texture2D.ResourceMinLODClamp = 0.0f;
	srvDesc[0].Format = normalSmoothnessBuffer.GetDesc().Format;
	srvDesc[0].Texture2D.MipLevels = normalSmoothnessBuffer.GetDesc().MipLevels;
	
	// Fill depth buffer descriptor
	srvDesc[1].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc[1].ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc[1].Texture2D.MostDetailedMip = 0;
	srvDesc[1].Texture2D.ResourceMinLODClamp = 0.0f;
	srvDesc[1].Format = SettingsManager::sDepthStencilSRVFormat;
	srvDesc[1].Texture2D.MipLevels = depthBuffer.GetDesc().MipLevels;

	// Fill sample kernel buffer descriptor
	srvDesc[2].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc[2].Format = mSampleKernelBuffer->GetResource()->GetDesc().Format;
	srvDesc[2].ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	srvDesc[2].Buffer.FirstElement = 0UL;
	srvDesc[2].Buffer.NumElements = mSampleKernelSize;
	srvDesc[2].Buffer.StructureByteStride = sizeof(XMFLOAT4);

	// Fill kernel noise texture descriptor
	srvDesc[3].Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc[3].ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc[3].Texture2D.MostDetailedMip = 0;
	srvDesc[3].Texture2D.ResourceMinLODClamp = 0.0f;
	srvDesc[3].Format = noiseTexture->GetDesc().Format;
	srvDesc[3].Texture2D.MipLevels = noiseTexture->GetDesc().MipLevels;

	// Create SRVs
	mPixelShaderBuffersGpuDesc = CbvSrvUavDescriptorManager::CreateShaderResourceViews(res, srvDesc, _countof(srvDesc));
}