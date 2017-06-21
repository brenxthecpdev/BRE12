#pragma once

#include <GeometryPass/GeometryCommandListRecorder.h>

namespace BRE {
///
/// @brief Responsible to record command lists that implement normal mapping
///
class NormalMappingCommandListRecorder : public GeometryCommandListRecorder {
public:
    NormalMappingCommandListRecorder() = default;
    ~NormalMappingCommandListRecorder() = default;
    NormalMappingCommandListRecorder(const NormalMappingCommandListRecorder&) = delete;
    const NormalMappingCommandListRecorder& operator=(const NormalMappingCommandListRecorder&) = delete;
    NormalMappingCommandListRecorder(NormalMappingCommandListRecorder&&) = default;
    NormalMappingCommandListRecorder& operator=(NormalMappingCommandListRecorder&&) = default;

    ///
    /// @brief Initializes pipeline state object and root signature
    /// @param geometryBufferFormats List of geometry buffers formats. It must be not nullptr
    /// @param geometryBufferCount Number of geometry buffers formats in @p geometryBufferFormats
    ///
    static void InitSharedPSOAndRootSignature(const DXGI_FORMAT* geometryBufferFormats,
                                              const std::uint32_t geometryBufferCount) noexcept;

    ///
    /// @brief Initializes the recorder
    ///
    /// InitSharedPSOAndRootSignature() must be called first
    ///
    /// @param geometryDataVector List of geometry data. Must not be empty
    /// @param baseColorTextures List of base color textures. Must not be empty.
    /// @param metalnessTextures List of metalness textures. Must not be empty.
    /// @param roughnessTextures List of rougness textures. Must not be empty.
    /// @param normalTextures List of normal textures. Must not be empty.
    ///
    void Init(const std::vector<GeometryData>& geometryDataVector,
              const std::vector<ID3D12Resource*>& baseColorTextures,
              const std::vector<ID3D12Resource*>& metalnessTextures,
              const std::vector<ID3D12Resource*>& roughnessTextures,
              const std::vector<ID3D12Resource*>& normalTextures) noexcept;

    ///
    /// @brief Records and push command lists to CommandListExecutor
    ///
    /// Init() must be called first
    ///
    /// @param frameCBuffer Constant buffer per frame, for current frame
    /// @return The number of pushed command lists
    ///
    std::uint32_t RecordAndPushCommandLists(const FrameCBuffer& frameCBuffer) noexcept final override;

    ///
    /// @brief Checks if internal data is valid. Typically, used for assertions
    /// @return True if valid. Otherwise, false
    ///
    bool IsDataValid() const noexcept final override;

private:
    ///
    /// @brief Initializes the constant buffers and views
    /// @param baseColorTextures List of base color textures. Must not be empty.
    /// @param metalnessTextures List of metalness textures. Must not be empty.
    /// @param roughnessTextures List of rougness textures. Must not be empty.
    /// @param normalTextures List of normal textures. Must not be empty.
    ///
    void InitCBuffersAndViews(const std::vector<ID3D12Resource*>& baseColorTextures,
                              const std::vector<ID3D12Resource*>& metalnessTextures,
                              const std::vector<ID3D12Resource*>& roughnessTextures,
                              const std::vector<ID3D12Resource*>& normalTextures) noexcept;

    // First descriptor in the list. All the others are contiguous
    D3D12_GPU_DESCRIPTOR_HANDLE mBaseColorTextureRenderTargetViewsBegin{ 0U };

    // First descriptor in the list. All the others are contiguous
    D3D12_GPU_DESCRIPTOR_HANDLE mMetalnessTextureRenderTargetViewsBegin{ 0U };

    // First descriptor in the list. All the others are contiguous
    D3D12_GPU_DESCRIPTOR_HANDLE mRoughnessTextureRenderTargetViewsBegin{ 0U };

    // First descriptor in the list. All the others are contiguous
    D3D12_GPU_DESCRIPTOR_HANDLE mNormalTextureRenderTargetViewsBegin{ 0U };
};
}