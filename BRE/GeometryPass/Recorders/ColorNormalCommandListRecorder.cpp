#include "ColorNormalCommandListRecorder.h"

#include <DirectXMath.h>

#include <CommandListExecutor\CommandListExecutor.h>
#include <DescriptorManager\CbvSrvUavDescriptorManager.h>
#include <DirectXManager/DirectXManager.h>
#include <MathUtils/MathUtils.h>
#include <PSOManager/PSOManager.h>
#include <ResourceManager/UploadBufferManager.h>
#include <RootSignatureManager\RootSignatureManager.h>
#include <ShaderManager\ShaderManager.h>
#include <ShaderUtils\CBuffers.h>
#include <ShaderUtils\MaterialProperties.h>
#include <Utils/DebugUtils.h>
#include "NormalCommandListRecorder.h"

namespace BRE {
// Root Signature:
// "DescriptorTable(CBV(b0), visibility = SHADER_VISIBILITY_VERTEX), " \ 0 -> Object CBuffers
// "CBV(b1, visibility = SHADER_VISIBILITY_VERTEX), " \ 1 -> Frame CBuffers
// "DescriptorTable(CBV(b0), visibility = SHADER_VISIBILITY_PIXEL), " \ 2 -> Material CBuffers
// "CBV(b1, visibility = SHADER_VISIBILITY_PIXEL), " \ 3 -> Frame CBuffer
// "DescriptorTable(SRV(t0), visibility = SHADER_VISIBILITY_PIXEL), " \ 4 -> Normal Texture

namespace {
ID3D12PipelineState* sPSO{ nullptr };
ID3D12RootSignature* sRootSignature{ nullptr };
}

void
ColorNormalCommandListRecorder::InitSharedPSOAndRootSignature(const DXGI_FORMAT* geometryBufferFormats,
                                                              const std::uint32_t geometryBufferCount) noexcept
{
    BRE_ASSERT(geometryBufferFormats != nullptr);
    BRE_ASSERT(geometryBufferCount > 0U);
    BRE_ASSERT(sPSO == nullptr);
    BRE_ASSERT(sRootSignature == nullptr);

    // Build pso and root signature
    PSOManager::PSOCreationData psoData{};
    psoData.mInputLayoutDescriptors = D3DFactory::GetPositionNormalTangentTexCoordInputLayout();

    psoData.mPixelShaderBytecode = ShaderManager::LoadShaderFileAndGetBytecode("GeometryPass/Shaders/ColorNormalMapping/PS.cso");
    psoData.mVertexShaderBytecode = ShaderManager::LoadShaderFileAndGetBytecode("GeometryPass/Shaders/ColorNormalMapping/VS.cso");

    ID3DBlob* rootSignatureBlob = &ShaderManager::LoadShaderFileAndGetBlob("GeometryPass/Shaders/ColorNormalMapping/RS.cso");
    psoData.mRootSignature = &RootSignatureManager::CreateRootSignatureFromBlob(*rootSignatureBlob);
    sRootSignature = psoData.mRootSignature;

    psoData.mNumRenderTargets = geometryBufferCount;
    memcpy(psoData.mRenderTargetFormats,
           geometryBufferFormats,
           sizeof(DXGI_FORMAT) * psoData.mNumRenderTargets);
    sPSO = &PSOManager::CreateGraphicsPSO(psoData);

    BRE_ASSERT(sPSO != nullptr);
    BRE_ASSERT(sRootSignature != nullptr);
}

void
ColorNormalCommandListRecorder::Init(const std::vector<GeometryData>& geometryDataVector,
                                     const std::vector<MaterialProperties>& materialProperties,
                                     const std::vector<ID3D12Resource*>& normalTexturess) noexcept
{
    BRE_ASSERT(IsDataValid() == false);
    BRE_ASSERT(materialProperties.empty() == false);
    BRE_ASSERT(materialProperties.size() == normalTexturess.size());

    const std::size_t numResources = materialProperties.size();
    const std::size_t geometryDataCount = geometryDataVector.size();

    // Check that the total number of matrices (geometry to be drawn) will be equal to available materials
#ifdef _DEBUG
    std::size_t totalNumMatrices{ 0UL };
    for (std::size_t i = 0UL; i < geometryDataCount; ++i) {
        const std::size_t numMatrices{ geometryDataVector[i].mWorldMatrices.size() };
        totalNumMatrices += numMatrices;
        BRE_ASSERT(numMatrices != 0UL);
    }
    BRE_ASSERT(totalNumMatrices == numResources);
#endif
    mGeometryDataVec.reserve(geometryDataCount);
    for (std::uint32_t i = 0U; i < geometryDataCount; ++i) {
        mGeometryDataVec.push_back(geometryDataVector[i]);
    }

    InitConstantBuffers(materialProperties, normalTexturess);

    BRE_ASSERT(IsDataValid());
}

void
ColorNormalCommandListRecorder::RecordAndPushCommandLists(const FrameCBuffer& frameCBuffer) noexcept
{
    BRE_ASSERT(IsDataValid());
    BRE_ASSERT(sPSO != nullptr);
    BRE_ASSERT(sRootSignature != nullptr);
    BRE_ASSERT(mGeometryBufferRenderTargetViews != nullptr);
    BRE_ASSERT(mGeometryBufferRenderTargetViewCount != 0U);
    BRE_ASSERT(mDepthBufferView.ptr != 0U);

    // Update frame constants
    UploadBuffer& uploadFrameCBuffer(mFrameUploadCBufferPerFrame.GetNextFrameCBuffer());
    uploadFrameCBuffer.CopyData(0U, &frameCBuffer, sizeof(frameCBuffer));

    ID3D12GraphicsCommandList& commandList = mCommandListPerFrame.ResetCommandListWithNextCommandAllocator(sPSO);

    commandList.RSSetViewports(1U, &SettingsManager::sScreenViewport);
    commandList.RSSetScissorRects(1U, &SettingsManager::sScissorRect);
    commandList.OMSetRenderTargets(mGeometryBufferRenderTargetViewCount,
                                   mGeometryBufferRenderTargetViews,
                                   false,
                                   &mDepthBufferView);

    ID3D12DescriptorHeap* heaps[] = { &CbvSrvUavDescriptorManager::GetDescriptorHeap() };
    commandList.SetDescriptorHeaps(_countof(heaps), heaps);
    commandList.SetGraphicsRootSignature(sRootSignature);

    const std::size_t descHandleIncSize{ DirectXManager::GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) };
    D3D12_GPU_DESCRIPTOR_HANDLE objectCBufferGpuDesc(mStartObjectCBufferView);
    D3D12_GPU_DESCRIPTOR_HANDLE materialsCBufferGpuDesc(mStartMaterialCBufferView);
    D3D12_GPU_DESCRIPTOR_HANDLE normalsBufferGpuDesc(mNormalBufferGpuDescriptorsBegin);

    commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Set frame constants root parameters
    D3D12_GPU_VIRTUAL_ADDRESS frameCBufferGpuVAddress(uploadFrameCBuffer.GetResource()->GetGPUVirtualAddress());
    commandList.SetGraphicsRootConstantBufferView(1U, frameCBufferGpuVAddress);
    commandList.SetGraphicsRootConstantBufferView(3U, frameCBufferGpuVAddress);

    // Draw objects
    const std::size_t geomCount{ mGeometryDataVec.size() };
    for (std::size_t i = 0UL; i < geomCount; ++i) {
        GeometryData& geomData{ mGeometryDataVec[i] };
        commandList.IASetVertexBuffers(0U, 1U, &geomData.mVertexBufferData.mBufferView);
        commandList.IASetIndexBuffer(&geomData.mIndexBufferData.mBufferView);
        const std::size_t worldMatsCount{ geomData.mWorldMatrices.size() };
        for (std::size_t j = 0UL; j < worldMatsCount; ++j) {
            commandList.SetGraphicsRootDescriptorTable(0U, objectCBufferGpuDesc);
            objectCBufferGpuDesc.ptr += descHandleIncSize;

            commandList.SetGraphicsRootDescriptorTable(2U, materialsCBufferGpuDesc);
            materialsCBufferGpuDesc.ptr += descHandleIncSize;

            commandList.SetGraphicsRootDescriptorTable(4U, normalsBufferGpuDesc);
            normalsBufferGpuDesc.ptr += descHandleIncSize;

            commandList.DrawIndexedInstanced(geomData.mIndexBufferData.mElementCount, 1U, 0U, 0U, 0U);
        }
    }

    commandList.Close();

    CommandListExecutor::Get().AddCommandList(commandList);
}

bool
ColorNormalCommandListRecorder::IsDataValid() const noexcept
{
    const bool result =
        GeometryPassCommandListRecorder::IsDataValid() &&
        mNormalBufferGpuDescriptorsBegin.ptr != 0UL;

    return result;
}

void
ColorNormalCommandListRecorder::InitConstantBuffers(const std::vector<MaterialProperties>& materialProperties,
                                                    const std::vector<ID3D12Resource*>& normalTextures) noexcept
{
    BRE_ASSERT(materialProperties.empty() == false);
    BRE_ASSERT(materialProperties.size() == normalTextures.size());
    BRE_ASSERT(mObjectUploadCBuffers == nullptr);
    BRE_ASSERT(mMaterialUploadCBuffers == nullptr);

    const std::uint32_t numResources = static_cast<std::uint32_t>(materialProperties.size());

    // Create object cbuffer and fill it
    const std::size_t objCBufferElemSize{ UploadBuffer::GetRoundedConstantBufferSizeInBytes(sizeof(ObjectCBuffer)) };
    mObjectUploadCBuffers = &UploadBufferManager::CreateUploadBuffer(objCBufferElemSize, numResources);
    std::uint32_t k = 0U;
    const std::size_t geometryDataCount{ mGeometryDataVec.size() };
    ObjectCBuffer objCBuffer;
    for (std::size_t i = 0UL; i < geometryDataCount; ++i) {
        GeometryData& geomData{ mGeometryDataVec[i] };
        const std::uint32_t worldMatsCount{ static_cast<std::uint32_t>(geomData.mWorldMatrices.size()) };
        for (std::uint32_t j = 0UL; j < worldMatsCount; ++j) {
            MathUtils::StoreTransposeMatrix(geomData.mWorldMatrices[j], objCBuffer.mWorldMatrix);
            mObjectUploadCBuffers->CopyData(k + j, &objCBuffer, sizeof(objCBuffer));
        }

        k += worldMatsCount;
    }

    // Create materials cbuffer		
    const std::size_t matCBufferElemSize{ UploadBuffer::GetRoundedConstantBufferSizeInBytes(sizeof(MaterialProperties)) };
    mMaterialUploadCBuffers = &UploadBufferManager::CreateUploadBuffer(matCBufferElemSize, numResources);
    D3D12_GPU_VIRTUAL_ADDRESS materialsGpuAddress{ mMaterialUploadCBuffers->GetResource()->GetGPUVirtualAddress() };
    D3D12_GPU_VIRTUAL_ADDRESS objCBufferGpuAddress{ mObjectUploadCBuffers->GetResource()->GetGPUVirtualAddress() };

    // Create object / materials cbuffers descriptors
    // Create textures SRV descriptors
    std::vector<D3D12_CONSTANT_BUFFER_VIEW_DESC> objectCbufferViewDescVec;
    objectCbufferViewDescVec.reserve(numResources);

    std::vector<D3D12_CONSTANT_BUFFER_VIEW_DESC> materialCbufferViewDescVec;
    materialCbufferViewDescVec.reserve(numResources);

    std::vector<ID3D12Resource*> normalResVec;
    normalResVec.reserve(numResources);
    std::vector<D3D12_SHADER_RESOURCE_VIEW_DESC> normalSrvDescVec;
    normalSrvDescVec.reserve(numResources);
    for (std::size_t i = 0UL; i < numResources; ++i) {
        // Object cbuffer desc
        D3D12_CONSTANT_BUFFER_VIEW_DESC cBufferDesc{};
        cBufferDesc.BufferLocation = objCBufferGpuAddress + i * objCBufferElemSize;
        cBufferDesc.SizeInBytes = static_cast<std::uint32_t>(objCBufferElemSize);
        objectCbufferViewDescVec.push_back(cBufferDesc);

        // Material cbuffer desc
        cBufferDesc.BufferLocation = materialsGpuAddress + i * matCBufferElemSize;
        cBufferDesc.SizeInBytes = static_cast<std::uint32_t>(matCBufferElemSize);
        materialCbufferViewDescVec.push_back(cBufferDesc);

        // Normal descriptor
        normalResVec.push_back(normalTextures[i]);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        srvDesc.Format = normalResVec.back()->GetDesc().Format;
        srvDesc.Texture2D.MipLevels = normalResVec.back()->GetDesc().MipLevels;
        normalSrvDescVec.push_back(srvDesc);

        mMaterialUploadCBuffers->CopyData(static_cast<std::uint32_t>(i),
                                          &materialProperties[i],
                                          sizeof(MaterialProperties));
    }
    mStartObjectCBufferView =
        CbvSrvUavDescriptorManager::CreateConstantBufferViews(objectCbufferViewDescVec.data(),
                                                              static_cast<std::uint32_t>(objectCbufferViewDescVec.size()));
    mStartMaterialCBufferView =
        CbvSrvUavDescriptorManager::CreateConstantBufferViews(materialCbufferViewDescVec.data(),
                                                              static_cast<std::uint32_t>(materialCbufferViewDescVec.size()));
    mNormalBufferGpuDescriptorsBegin =
        CbvSrvUavDescriptorManager::CreateShaderResourceViews(normalResVec.data(),
                                                              normalSrvDescVec.data(),
                                                              static_cast<std::uint32_t>(normalSrvDescVec.size()));
}
}

