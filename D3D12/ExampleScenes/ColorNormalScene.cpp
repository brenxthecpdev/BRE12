#include "ColorNormalScene.h"

#include <tbb/parallel_for.h>

#include <GeometryPass/Recorders/ColorNormalCmdListRecorder.h>
#include <GlobalData/D3dData.h>
#include <LightingPass/PunctualLight.h>
#include <LightingPass/Recorders/PunctualLightCmdListRecorder.h>
#include <Material/Material.h>
#include <MathUtils\MathUtils.h>
#include <ModelManager\Mesh.h>
#include <ModelManager\ModelManager.h>
#include <ResourceManager\ResourceManager.h>

namespace {
	const char* sSkyBoxFile{ "textures/cubeMaps/milkmill_cube_map.dds" };
	const char* sDiffuseEnvironmentFile{ "textures/cubeMaps/milkmill_diffuse_cube_map.dds" };
	const char* sSpecularEnvironmentFile{ "textures/cubeMaps/milkmill_specular_cube_map.dds" };

	const float sS{ 2.0f };

	const float sTx{ 0.0f };
	const float sTy{ -3.5f };
	const float sTz{ 10.0f };
	const float sOffsetX{ 25.0f };

	const float sTx1{ 0.0f };
	const float sTy1{ -3.5f };
	const float sTz1{ 30.0f };
	const float sOffsetX1{ 25.0f };

	const float sTx2{ 0.0f };
	const float sTy2{ -3.5f };
	const float sTz2{ 15.0f };
	const float sOffsetX2{ 25.0f };

	const float sTx3{ 0.0f };
	const float sTy3{ -3.5f };
	const float sTz3{ 0.0f };
	const float sOffsetX3{ 15.0f };

	void GenerateRecorder(
		Microsoft::WRL::ComPtr<ID3D12Resource>* geometryBuffers,
		const std::uint32_t geometryBuffersCount,
		ID3D12Resource& depthBuffer,
		PunctualLightCmdListRecorder* &recorder) {
		recorder = new PunctualLightCmdListRecorder(D3dData::Device());
		PunctualLight light[1];
		light[0].mPosAndRange[0] = 0.0f;
		light[0].mPosAndRange[1] = 300.0f;
		light[0].mPosAndRange[2] = -100.0f;
		light[0].mPosAndRange[3] = 10000.0f;
		light[0].mColorAndPower[0] = 1.0f;
		light[0].mColorAndPower[1] = 1.0f;
		light[0].mColorAndPower[2] = 1.0f;
		light[0].mColorAndPower[3] = 1000000.0f;

		recorder->Init(
			geometryBuffers,
			geometryBuffersCount,
			depthBuffer,
			light,
			_countof(light));
	}

	void GenerateRecorder(
		const float initX,
		const float initY,
		const float initZ,
		const float offsetX,
		const float offsetY,
		const float offsetZ,
		const std::vector<Mesh>& meshes,
		ID3D12Resource** normals,
		Material* materials,
		const std::size_t numMaterials,
		ColorNormalCmdListRecorder* &recorder) {

		ASSERT(normals != nullptr);

		recorder = new ColorNormalCmdListRecorder(D3dData::Device());

		const std::size_t numMeshes{ meshes.size() };
		ASSERT(numMeshes > 0UL);

		std::vector<GeometryPassCmdListRecorder::GeometryData> geomDataVec;
		geomDataVec.resize(numMeshes);
		for (std::size_t i = 0UL; i < numMeshes; ++i) {
			GeometryPassCmdListRecorder::GeometryData& geomData{ geomDataVec[i] };
			const Mesh& mesh{ meshes[i] };
			geomData.mVertexBufferData = mesh.VertexBufferData();
			geomData.mIndexBufferData = mesh.IndexBufferData();
			geomData.mWorldMatrices.reserve(numMaterials);
		}

		std::vector<Material> materialsVec;
		materialsVec.resize(numMaterials * numMeshes);
		std::vector<ID3D12Resource*> texturesVec;
		texturesVec.resize(numMaterials * numMeshes);
		std::vector<ID3D12Resource*> normalsVec;
		normalsVec.resize(numMaterials * numMeshes);

		float tx{ initX };
		float ty{ initY };
		float tz{ initZ };
		for (std::size_t i = 0UL; i < numMaterials; ++i) {
			DirectX::XMFLOAT4X4 w;
			MathUtils::ComputeMatrix(w, tx, ty, tz, sS, sS, sS);

			Material& mat(materials[i]);
			ID3D12Resource* normal{ normals[i] };
			for (std::size_t j = 0UL; j < numMeshes; ++j) {
				const std::size_t index{ i + j * numMaterials };
				materialsVec[index] = mat;
				normalsVec[index] = normal;
				GeometryPassCmdListRecorder::GeometryData& geomData{ geomDataVec[j] };
				geomData.mWorldMatrices.push_back(w);
			}

			tx += offsetX;
			ty += offsetY;
			tz += offsetZ;
		}

		recorder->Init(
			geomDataVec.data(),
			static_cast<std::uint32_t>(geomDataVec.size()),
			materialsVec.data(),
			normalsVec.data(),
			static_cast<std::uint32_t>(materialsVec.size()));
	}
}

void ColorNormalScene::GenerateGeomPassRecorders(
	ID3D12CommandQueue& cmdQueue,
	std::vector<std::unique_ptr<GeometryPassCmdListRecorder>>& tasks) noexcept {

	ASSERT(tasks.empty());
	ASSERT(ValidateData());

	CHECK_HR(mCmdList->Reset(mCmdAlloc, nullptr));

	Model* model;
	Microsoft::WRL::ComPtr<ID3D12Resource> uploadVertexBuffer;
	Microsoft::WRL::ComPtr<ID3D12Resource> uploadIndexBuffer;
	ModelManager::Get().CreateSphere(2, 50, 50, model, *mCmdList, uploadVertexBuffer, uploadIndexBuffer);
	ASSERT(model != nullptr);

	Model* model1;
	Microsoft::WRL::ComPtr<ID3D12Resource> uploadVertexBuffer1;
	Microsoft::WRL::ComPtr<ID3D12Resource> uploadIndexBuffer1;
	ModelManager::Get().LoadModel("models/mitsubaFloor.obj", model1, *mCmdList, uploadVertexBuffer1, uploadIndexBuffer1);
	ASSERT(model1 != nullptr);

	const std::uint32_t numResources{ 6U };

	Material materials[numResources];
	for (std::uint32_t i = 0UL; i < numResources; ++i) {
		materials[i].RandomBaseColor();
		materials[i].mBaseColor_MetalMask[3U] = 0.0f;
		materials[i].mSmoothness = 0.7f;
	}

	materials[4].mSmoothness = 0.2f;
	materials[5].mSmoothness = 0.2f;
	
	ID3D12Resource* normal[numResources];
	Microsoft::WRL::ComPtr<ID3D12Resource> uploadBufferNormal[numResources];
	ResourceManager::Get().LoadTextureFromFile("textures/rock/rock_normal.dds", normal[0], uploadBufferNormal[0], *mCmdList);
	ASSERT(normal[0] != nullptr);
	ResourceManager::Get().LoadTextureFromFile("textures/rock/rock2_normal.dds", normal[1], uploadBufferNormal[1], *mCmdList);
	ASSERT(normal[1] != nullptr);
	ResourceManager::Get().LoadTextureFromFile("textures/wood/wood_normal.dds", normal[2], uploadBufferNormal[2], *mCmdList);
	ASSERT(normal[2] != nullptr);
	ResourceManager::Get().LoadTextureFromFile("textures/floor_normal.dds", normal[3], uploadBufferNormal[3], *mCmdList);
	ASSERT(normal[3] != nullptr);
	ResourceManager::Get().LoadTextureFromFile("textures/sand/sand_normal.dds", normal[4], uploadBufferNormal[4], *mCmdList);
	ASSERT(normal[4] != nullptr);
	ResourceManager::Get().LoadTextureFromFile("textures/cobblestone/cobblestone_normal.dds", normal[5], uploadBufferNormal[5], *mCmdList);
	ASSERT(normal[5] != nullptr);

	ExecuteCommandList(cmdQueue);

	tasks.resize(2);

	ColorNormalCmdListRecorder* recorder{ nullptr };
	GenerateRecorder(sTx1, sTy1, sTz1, sOffsetX1, 0.0f, 0.0f, model1->Meshes(), normal, materials, numResources, recorder);
	ASSERT(recorder != nullptr);
	tasks[0].reset(recorder);

	ColorNormalCmdListRecorder* recorder2{ nullptr };
	GenerateRecorder(sTx2, sTy2, sTz2, sOffsetX2, 0.0f, 0.0f, model->Meshes(), normal, materials, numResources, recorder2);
	ASSERT(recorder2 != nullptr);
	tasks[1].reset(recorder2);
}

void ColorNormalScene::GenerateLightingPassRecorders(
	Microsoft::WRL::ComPtr<ID3D12Resource>* geometryBuffers,
	const std::uint32_t geometryBuffersCount,
	ID3D12Resource& depthBuffer,
	std::vector<std::unique_ptr<LightingPassCmdListRecorder>>& tasks) noexcept
{
	ASSERT(tasks.empty());
	ASSERT(geometryBuffers != nullptr);
	ASSERT(0 < geometryBuffersCount && geometryBuffersCount < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
	ASSERT(ValidateData());

	tasks.resize(1UL);
	PunctualLightCmdListRecorder* recorder{ nullptr };
	GenerateRecorder(
		geometryBuffers,
		geometryBuffersCount,
		depthBuffer,
		recorder);
	ASSERT(recorder != nullptr);
	tasks[0].reset(recorder);
}

void ColorNormalScene::GenerateCubeMaps(
	ID3D12CommandQueue& cmdQueue,
	ID3D12Resource* &skyBoxCubeMap,
	ID3D12Resource* &diffuseIrradianceCubeMap,
	ID3D12Resource* &specularPreConvolvedCubeMap) noexcept
{
	CHECK_HR(mCmdList->Reset(mCmdAlloc, nullptr));

	// Cube map textures
	Microsoft::WRL::ComPtr<ID3D12Resource> uploadBufferTex;
	ResourceManager::Get().LoadTextureFromFile(sDiffuseEnvironmentFile, diffuseIrradianceCubeMap, uploadBufferTex, *mCmdList);
	ASSERT(diffuseIrradianceCubeMap != nullptr);

	Microsoft::WRL::ComPtr<ID3D12Resource> uploadBufferTex2;
	ResourceManager::Get().LoadTextureFromFile(sSpecularEnvironmentFile, specularPreConvolvedCubeMap, uploadBufferTex2, *mCmdList);
	ASSERT(specularPreConvolvedCubeMap != nullptr);

	Microsoft::WRL::ComPtr<ID3D12Resource> uploadBufferTex3;
	ResourceManager::Get().LoadTextureFromFile(sSkyBoxFile, skyBoxCubeMap, uploadBufferTex3, *mCmdList);
	ASSERT(skyBoxCubeMap != nullptr);

	ExecuteCommandList(cmdQueue);
}

