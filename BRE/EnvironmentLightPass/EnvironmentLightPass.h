#pragma once

#include <memory>
#include <wrl.h>

#include <EnvironmentLightPass\AmbientOcclusionCmdListRecorder.h>
#include <EnvironmentLightPass\BlurCmdListRecorder.h>
#include <EnvironmentLightPass\EnvironmentLightCmdListRecorder.h>
#include <CommandManager\CommandListPerFrame.h>

struct D3D12_CPU_DESCRIPTOR_HANDLE;
struct ID3D12Resource;

namespace BRE {
///
/// @brief Pass responsible to apply ambient lighting and ambient occlusion
/// 
class EnvironmentLightPass {
public:
    EnvironmentLightPass() = default;
    ~EnvironmentLightPass() = default;
    EnvironmentLightPass(const EnvironmentLightPass&) = delete;
    const EnvironmentLightPass& operator=(const EnvironmentLightPass&) = delete;
    EnvironmentLightPass(EnvironmentLightPass&&) = delete;
    EnvironmentLightPass& operator=(EnvironmentLightPass&&) = delete;

    ///
    /// @brief Initializes the pass
    /// @param baseColorMetalMaskBuffer Geometry buffer that contains base color and metal mask.
    /// @param normalSmoothnessBuffer Geometry buffer that contains normals and smoothness factors.
    /// @param depthBuffer Depth buffer
    /// @param diffuseIrradianceCubeMap Diffuse irradiance environment cube map
    /// @param specularPreConvolvedCubeMap Specular pre convolved environment cube map
    ///
    void Init(ID3D12Resource& baseColorMetalMaskBuffer,
              ID3D12Resource& normalSmoothnessBuffer,
              ID3D12Resource& depthBuffer,
              ID3D12Resource& diffuseIrradianceCubeMap,
              ID3D12Resource& specularPreConvolvedCubeMap,
              const D3D12_CPU_DESCRIPTOR_HANDLE& renderTargetView) noexcept;

    ///
    /// @brief Executes the pass
    /// @param frameCBuffer Constant buffer per frame, for current frame
    ///
    void Execute(const FrameCBuffer& frameCBuffer) noexcept;

private:
    ///
    /// @brief Validates internal data. Typically, used with assertions
    ///
    bool ValidateData() const noexcept;

    ///
    /// @brief Executes begin task for the pass
    ///
    void ExecuteBeginTask() noexcept;

    ///
    /// @brief Executes middle task for the pass
    ///
    void ExecuteMiddleTask() noexcept;

    ///
    /// Executes final task for the pass
    ///
    void ExecuteFinalTask() noexcept;

    CommandListPerFrame mBeginCommandListPerFrame;
    CommandListPerFrame mMiddleCommandListPerFrame;
    CommandListPerFrame mFinalCommandListPerFrame;

    Microsoft::WRL::ComPtr<ID3D12Resource> mAmbientAccessibilityBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE mAmbientAccessibilityBufferRenderTargetView{ 0UL };

    Microsoft::WRL::ComPtr<ID3D12Resource> mBlurBuffer;

    std::unique_ptr<AmbientOcclusionCmdListRecorder> mAmbientOcclusionRecorder;
    std::unique_ptr<BlurCmdListRecorder> mBlurRecorder;
    std::unique_ptr<EnvironmentLightCmdListRecorder> mEnvironmentLightRecorder;
};

}

