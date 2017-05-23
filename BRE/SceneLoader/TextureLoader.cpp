#include "TextureLoader.h"

#include <d3d12.h>
#include <vector>
#pragma warning( push )
#pragma warning( disable : 4127)
#include <yaml-cpp/yaml.h>
#pragma warning( pop ) 

#include <CommandListExecutor\CommandListExecutor.h>
#include <ResourceManager\ResourceManager.h>
#include <Utils/DebugUtils.h>

namespace BRE {
void
TextureLoader::LoadTextures(const YAML::Node& rootNode,
                            ID3D12CommandAllocator& commandAllocator,
                            ID3D12GraphicsCommandList& commandList) noexcept
{
    BRE_ASSERT(rootNode.IsDefined());

    // Get the "textures" node. It is a map and its sintax is:
    // textures:
    //   name1: path1
    //   name2: path2
    //   name3: path3
    const YAML::Node texturesNode = rootNode["textures"];

    // 'textures' node can be undefined
    if (texturesNode.IsDefined() == false) {
        return;
    }

    BRE_ASSERT_MSG(texturesNode.IsMap(), L"'textures' node must be a map");

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploadBuffers;
    BRE_CHECK_HR(commandList.Reset(&commandAllocator, nullptr));
    
    LoadTextures(texturesNode,
                 commandAllocator,
                 commandList,
                 uploadBuffers);

    commandList.Close();
    CommandListExecutor::Get().ExecuteCommandListAndWaitForCompletion(commandList);    
}

ID3D12Resource&
TextureLoader::GetTexture(const std::string& name) noexcept
{
    std::unordered_map<std::string, ID3D12Resource*>::iterator findIt = mTextureByName.find(name);
    BRE_ASSERT_MSG(findIt != mTextureByName.end(), L"Texture name not found");
    BRE_ASSERT(findIt->second != nullptr);

    return *findIt->second;
}

void
TextureLoader::LoadTextures(const YAML::Node& texturesNode,
                            ID3D12CommandAllocator& commandAllocator,
                            ID3D12GraphicsCommandList& commandList,
                            std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadBuffers) noexcept
{
    BRE_ASSERT_MSG(texturesNode.IsMap(), L"'textures' node must be a map");

    std::string name;
    std::string path;
    for (YAML::const_iterator it = texturesNode.begin(); it != texturesNode.end(); ++it) {
        name = it->first.as<std::string>();
        path = it->second.as<std::string>();

        // If name is "reference", then path must be a yaml file that specifies "textures"
        if (name == "reference") {
            const YAML::Node referenceRootNode = YAML::LoadFile(path);
            BRE_ASSERT_MSG(referenceRootNode.IsDefined(), L"Failed to open yaml file");
            const YAML::Node referenceTexturesNode = referenceRootNode["textures"];
            LoadTextures(referenceTexturesNode,
                         commandAllocator,
                         commandList,
                         uploadBuffers);
        } else {
            BRE_ASSERT_MSG(mTextureByName.find(name) == mTextureByName.end(), L"Texture name is not unique");

            uploadBuffers.resize(uploadBuffers.size() + 1);

            ID3D12Resource& texture = ResourceManager::LoadTextureFromFile(path.c_str(),
                                                                           commandList,
                                                                           uploadBuffers.back(),
                                                                           nullptr);

            mTextureByName[name] = &texture;
        }
    }
}
}