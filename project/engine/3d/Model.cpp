// 役割: モデルのメッシュ描画、材質設定、Skinning描画を実装する。
#include "Model.h"
#include "ModelCommon.h"
#include "ModelFormat.h"
#include "PmxLoader.h"
#include "AssimpUnicodeIO.h"
#include "../base/DirectXCommon.h"
#include "../2d/TextureManager.h"
#include "../utility/Logger.h"
#include "../utility/EditableResourcePath.h"
#include "../utility/StringUtility.h"
#include <DirectXTex.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <filesystem>
#include <exception>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <utility>

namespace {

constexpr const char* kDefaultModelTexture = "resources/human/white.png";

bool IsGrayscalePng(const uint8_t* data, size_t size) {
	// PNGシグネチャに続くIHDRのカラータイプ0だけを対象にする。
	constexpr uint8_t header[] = {
		137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R'
	};
	return data && size >= 33 &&
		std::memcmp(data, header, sizeof(header)) == 0 && data[25] == 0;
}

bool RegisterEmbeddedTexture(TextureManager& textures, const Model::EmbeddedTextureSource& source) {
	const auto* data = source.bytes.data();
	const size_t size = source.bytes.size();
	if (!source.expandGrayscaleBaseColor) {
		return textures.LoadTextureFromMemory(source.key, data, size, source.formatHint);
	}
	if (textures.HasTexture(source.key)) return true;

	TextureManager::DecodedTexture color;
	switch (TextureManager::TryDecodeGrayscaleBaseColorPng(data, size, color)) {
	case TextureManager::GrayscaleBaseColorDecodeStatus::Expanded: {
		const auto& metadata = color.metadata;
		return textures.UpdateTextureFromPixels(source.key, color.image.GetPixels(),
			static_cast<uint32_t>(metadata.width), static_cast<uint32_t>(metadata.height), metadata.format);
	}
	case TextureManager::GrayscaleBaseColorDecodeStatus::UseOriginalPng:
		return textures.LoadTextureFromMemory(source.key, data, size, "png");
	case TextureManager::GrayscaleBaseColorDecodeStatus::Failed:
		return false;
	}
	return false;
}

// 同名のディレクトリを持つ別の場所のモデルは従来の経路で読む。
bool IsAlleyResource(const std::filesystem::path& modelPath) {
	const auto alleyRoot = EditableResourcePath::ResolveResource("alley");
	if (alleyRoot.empty()) {
		return false;
	}
	const auto relative = modelPath.lexically_relative(alleyRoot);
	return !relative.empty() && *relative.begin() != ".." &&
		*relative.begin() != ".";
}

struct StaticMeshInstance {
	uint32_t meshIndex;
	aiMatrix4x4 transform;
	aiMatrix3x3 normalTransform;
	bool reverseWinding;
};

bool IsFiniteVector(const aiVector3D& value) {
	return std::isfinite(value.x) && std::isfinite(value.y) &&
		std::isfinite(value.z);
}

Matrix4x4 ConvertAssimpMatrix(const aiMatrix4x4& matrix) {
	// Keep the inverse bind pose exact while converting RH/column-vector data.
	const float source[4][4] = {
		{ matrix.a1, matrix.a2, matrix.a3, matrix.a4 },
		{ matrix.b1, matrix.b2, matrix.b3, matrix.b4 },
		{ matrix.c1, matrix.c2, matrix.c3, matrix.c4 },
		{ matrix.d1, matrix.d2, matrix.d3, matrix.d4 }
	};
	const float handedness[4] = { -1.0f, 1.0f, 1.0f, 1.0f };

	Matrix4x4 result{};
	for (uint32_t row = 0; row < 4; ++row) {
		for (uint32_t column = 0; column < 4; ++column) {
			result.m[row][column] =
				handedness[row] *
				source[column][row] *
				handedness[column];
		}
	}
	return result;
}

} // namespace

bool Model::Initialize(
	ModelCommon* modelCommon,
	const std::filesystem::path& modelFilePath
) {
	this->modelCommon = modelCommon;
	PreparedModelData prepared;
	std::vector<std::string> diagnostics;
	const bool parsed = TryPrepareModelData(
		modelFilePath, IsAlleyResource(modelFilePath.lexically_normal()), prepared, diagnostics);
	for (const auto& diagnostic : diagnostics) Logger::Log(diagnostic);
	if (!parsed) return false;

	auto* textureManager = TextureManager::GetInstance();
	// 材質順で登録し、成功した画像だけを最終材質に反映する。
	for (const auto& binding : prepared.embeddedBindings) {
		const auto& source = prepared.embeddedTextures[binding.sourceIndex];
		if (RegisterEmbeddedTexture(*textureManager, source)) {
			prepared.model.materials[binding.materialIndex].textureFilePath = source.key;
		} else if (source.expandGrayscaleBaseColor) {
			Logger::Log("Failed to expand grayscale model base color: " + source.key + "\n");
		}
	}
	modelData = std::move(prepared.model);
	animations_ = std::move(prepared.animations);
	CreateVertexResource();
	CreateIndexResource();
	CreateMaterialResource();

	for (MaterialSlot& material : modelData.materials) {
		if (
			!material.textureFilePath.empty() &&
			textureManager->LoadTexture(material.textureFilePath)
		) {
			continue;
		}

		// テクスチャ未指定は材質色をそのまま使える白画像へ、読込失敗はUVチェッカーへ退避する。
		const bool hasSpecifiedTexture = !material.textureFilePath.empty();
		material.textureFilePath = hasSpecifiedTexture
			? "resources/uvChecker.png"
			: kDefaultModelTexture;
		textureManager->LoadTexture(material.textureFilePath);
	}
	return true;
}
void Model::Draw(const D3D12_VERTEX_BUFFER_VIEW* influenceBufferView) {
	DrawWithMaterial(nullptr, influenceBufferView);
}

bool Model::GetLocalBounds(Vector3& boundsMin, Vector3& boundsMax) const {
	if (modelData.vertices.empty()) {
		boundsMin = {};
		boundsMax = {};
		return false;
	}

	const float maximum = (std::numeric_limits<float>::max)();
	boundsMin = { maximum, maximum, maximum };
	boundsMax = { -maximum, -maximum, -maximum };
	for (const VertexData& vertex : modelData.vertices) {
		boundsMin.x = (std::min)(boundsMin.x, vertex.position.x);
		boundsMin.y = (std::min)(boundsMin.y, vertex.position.y);
		boundsMin.z = (std::min)(boundsMin.z, vertex.position.z);
		boundsMax.x = (std::max)(boundsMax.x, vertex.position.x);
		boundsMax.y = (std::max)(boundsMax.y, vertex.position.y);
		boundsMax.z = (std::max)(boundsMax.z, vertex.position.z);
	}
	return true;
}

void Model::DrawWithMaterial(
	ID3D12Resource* materialOverride,
	const D3D12_VERTEX_BUFFER_VIEW* influenceBufferView
) {
	DrawWithMaterialAndTexture(materialOverride, {}, influenceBufferView);
}

void Model::DrawWithMaterialAndTexture(
	ID3D12Resource* materialOverride,
	D3D12_GPU_DESCRIPTOR_HANDLE textureOverride,
	const D3D12_VERTEX_BUFFER_VIEW* influenceBufferView
) {
	auto* commandList = modelCommon->GetDxCommon()->GetCommandList();
	if (influenceBufferView) {
		const D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[] = {
			vertexBufferView,
			*influenceBufferView
		};
		commandList->IASetVertexBuffers(0, 2, vertexBufferViews);
	}
	else {
		commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
	}
	commandList->IASetIndexBuffer(&indexBufferView);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// マテリアルCBufferの場所を設定
	ID3D12Resource* materialToBind =
		materialOverride ? materialOverride : nullptr;
	for (const SubMesh& subMesh : modelData.subMeshes) {
		const uint32_t materialIndex = (std::min)(
			subMesh.materialIndex,
			static_cast<uint32_t>(materialResources.size() - 1)
		);
		ID3D12Resource* subMeshMaterial = materialToBind
			? materialToBind
			: materialResources[materialIndex];
		commandList->SetGraphicsRootConstantBufferView(
			0,
			subMeshMaterial->GetGPUVirtualAddress()
		);
		const D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = textureOverride.ptr != 0
			? textureOverride
			: TextureManager::GetInstance()->GetSrvHandleGPU(
				modelData.materials[materialIndex].textureFilePath
			);
		commandList->SetGraphicsRootDescriptorTable(2, textureHandle);
		commandList->DrawIndexedInstanced(
			subMesh.indexCount, 1, subMesh.firstIndex, 0, 0
		);
	}
}

void Model::DrawWithMaterialSlots(
	const std::vector<ID3D12Resource*>& materialOverrides,
	const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& textureOverrides,
	const D3D12_VERTEX_BUFFER_VIEW* influenceBufferView
) {
	auto* commandList = modelCommon->GetDxCommon()->GetCommandList();
	if (influenceBufferView) {
		const D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[] = {
			vertexBufferView, *influenceBufferView
		};
		commandList->IASetVertexBuffers(0, 2, vertexBufferViews);
	} else {
		commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
	}
	commandList->IASetIndexBuffer(&indexBufferView);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	for (const SubMesh& subMesh : modelData.subMeshes) {
		const uint32_t materialIndex = (std::min)(
			subMesh.materialIndex,
			static_cast<uint32_t>(materialResources.size() - 1)
		);
		ID3D12Resource* material = materialIndex < materialOverrides.size() &&
			materialOverrides[materialIndex]
			? materialOverrides[materialIndex]
			: materialResources[materialIndex];
		const D3D12_GPU_DESCRIPTOR_HANDLE textureOverride =
			materialIndex < textureOverrides.size()
				? textureOverrides[materialIndex]
				: D3D12_GPU_DESCRIPTOR_HANDLE{};
		const D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = textureOverride.ptr != 0
			? textureOverride
			: TextureManager::GetInstance()->GetSrvHandleGPU(
				modelData.materials[materialIndex].textureFilePath
			);
		commandList->SetGraphicsRootConstantBufferView(
			0, material->GetGPUVirtualAddress()
		);
		commandList->SetGraphicsRootDescriptorTable(2, textureHandle);
		commandList->DrawIndexedInstanced(
			subMesh.indexCount, 1, subMesh.firstIndex, 0, 0
		);
	}
}

void Model::DrawForShadow(
	const D3D12_VERTEX_BUFFER_VIEW* influenceBufferView
) {
	auto* commandList = modelCommon->GetDxCommon()->GetCommandList();
	if (influenceBufferView) {
		const D3D12_VERTEX_BUFFER_VIEW vertexBufferViews[] = {
			vertexBufferView,
			*influenceBufferView
		};
		commandList->IASetVertexBuffers(0, 2, vertexBufferViews);
	}
	else {
		commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
	}
	commandList->IASetIndexBuffer(&indexBufferView);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	for (const SubMesh& subMesh : modelData.subMeshes) {
		commandList->DrawIndexedInstanced(
			subMesh.indexCount, 1, subMesh.firstIndex, 0, 0
		);
	}
}

void Model::DrawWithVertexBufferAndMaterial(
	const D3D12_VERTEX_BUFFER_VIEW& customVertexBufferView,
	ID3D12Resource* materialOverride,
	D3D12_GPU_DESCRIPTOR_HANDLE textureOverride
) {
	auto* commandList = modelCommon->GetDxCommon()->GetCommandList();
	commandList->IASetVertexBuffers(0, 1, &customVertexBufferView);
	commandList->IASetIndexBuffer(&indexBufferView);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	for (const SubMesh& subMesh : modelData.subMeshes) {
		const uint32_t materialIndex = (std::min)(
			subMesh.materialIndex,
			static_cast<uint32_t>(materialResources.size() - 1)
		);
		ID3D12Resource* materialToBind = materialOverride
			? materialOverride
			: materialResources[materialIndex];
		const D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = textureOverride.ptr != 0
			? textureOverride
			: TextureManager::GetInstance()->GetSrvHandleGPU(
				modelData.materials[materialIndex].textureFilePath
			);
		commandList->SetGraphicsRootConstantBufferView(
			0, materialToBind->GetGPUVirtualAddress()
		);
		commandList->SetGraphicsRootDescriptorTable(2, textureHandle);
		commandList->DrawIndexedInstanced(
			subMesh.indexCount, 1, subMesh.firstIndex, 0, 0
		);
	}
}

void Model::DrawWithVertexBufferAndMaterialSlots(
	const D3D12_VERTEX_BUFFER_VIEW& customVertexBufferView,
	const std::vector<ID3D12Resource*>& materialOverrides,
	const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& textureOverrides
) {
	auto* commandList = modelCommon->GetDxCommon()->GetCommandList();
	commandList->IASetVertexBuffers(0, 1, &customVertexBufferView);
	commandList->IASetIndexBuffer(&indexBufferView);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	for (const SubMesh& subMesh : modelData.subMeshes) {
		const uint32_t materialIndex = (std::min)(
			subMesh.materialIndex,
			static_cast<uint32_t>(materialResources.size() - 1)
		);
		ID3D12Resource* material = materialIndex < materialOverrides.size() &&
			materialOverrides[materialIndex]
			? materialOverrides[materialIndex]
			: materialResources[materialIndex];
		const D3D12_GPU_DESCRIPTOR_HANDLE textureOverride =
			materialIndex < textureOverrides.size()
				? textureOverrides[materialIndex]
				: D3D12_GPU_DESCRIPTOR_HANDLE{};
		const D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = textureOverride.ptr != 0
			? textureOverride
			: TextureManager::GetInstance()->GetSrvHandleGPU(
				modelData.materials[materialIndex].textureFilePath
			);
		commandList->SetGraphicsRootConstantBufferView(
			0, material->GetGPUVirtualAddress()
		);
		commandList->SetGraphicsRootDescriptorTable(2, textureHandle);
		commandList->DrawIndexedInstanced(
			subMesh.indexCount, 1, subMesh.firstIndex, 0, 0
		);
	}
}

void Model::DrawForShadowWithVertexBuffer(const D3D12_VERTEX_BUFFER_VIEW& customVertexBufferView) {
	auto* commandList = modelCommon->GetDxCommon()->GetCommandList();
	commandList->IASetVertexBuffers(0, 1, &customVertexBufferView);
	commandList->IASetIndexBuffer(&indexBufferView);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	for (const SubMesh& subMesh : modelData.subMeshes) {
		commandList->DrawIndexedInstanced(
			subMesh.indexCount, 1, subMesh.firstIndex, 0, 0
		);
	}
}

bool Model::TryPrepareModelData(const std::filesystem::path& resolvedModelPath,
	bool isAlleyResource, PreparedModelData& out, std::vector<std::string>& diagnostics) {
	diagnostics.clear();
	PreparedModelData candidate;
	if (!TryParseModelFile(resolvedModelPath, isAlleyResource, candidate, diagnostics)) return false;
	if (ModelFormat::NormalizeExtension(resolvedModelPath.extension().string()) != ".pmx") {
		std::string diagnostic;
		candidate.animations = ReadAnimationFiles(resolvedModelPath, diagnostic);
		if (!diagnostic.empty()) diagnostics.push_back(std::move(diagnostic));
	}
	out = std::move(candidate);
	return true;
}

bool Model::TryParseModelFile(const std::filesystem::path& requestedModelFilePath,
	bool isAlleyResource, PreparedModelData& candidate, std::vector<std::string>& diagnostics) {
	const std::filesystem::path modelFilePath =
		requestedModelFilePath.lexically_normal();
	const std::string filePath = StringUtility::ToUtf8(modelFilePath);
	const std::string extension = ModelFormat::NormalizeExtension(
		modelFilePath.extension().string()
	);
	if (ModelFormat::FindByExtension(extension) == nullptr) {
		diagnostics.push_back("Unsupported model format: " + filePath + "\n");
		return false;
	}
	const std::filesystem::path modelDirectory = modelFilePath.parent_path();
	if (extension == ".pmx") {
		PmxLoader::ModelData pmxData;
		std::string errorMessage;
		if (!PmxLoader::LoadBasic(modelFilePath, pmxData, errorMessage)) {
			diagnostics.push_back(
				"Failed to load PMX model: " + filePath +
				"\nPMX: " + errorMessage + "\n"
			);
			return false;
		}

		ModelData modelData;
		modelData.rootNode.name = pmxData.name.empty()
			? StringUtility::ToUtf8(modelFilePath.stem())
			: pmxData.name;
		modelData.rootNode.localMatrix = MakeIdentity4x4();

		// PMXの絶対座標ボーンを、既存Skeletonが扱う親相対Nodeへ変換する。
		std::vector<std::vector<size_t>> boneChildren(pmxData.bones.size());
		std::vector<size_t> rootBones;
		for (size_t boneIndex = 0; boneIndex < pmxData.bones.size(); ++boneIndex) {
			const int32_t parentIndex = pmxData.bones[boneIndex].parentIndex;
			if (parentIndex >= 0) {
				boneChildren[static_cast<size_t>(parentIndex)].push_back(boneIndex);
			}
			else {
				rootBones.push_back(boneIndex);
			}
		}

		std::function<Node(size_t)> buildBoneNode = [&](size_t boneIndex) {
			const PmxLoader::Bone& sourceBone = pmxData.bones[boneIndex];
			Node node{};
			node.name = sourceBone.name;
			node.transform.translate = sourceBone.position;
			if (sourceBone.parentIndex >= 0) {
				const Vector3& parentPosition =
					pmxData.bones[static_cast<size_t>(sourceBone.parentIndex)].position;
				node.transform.translate = {
					sourceBone.position.x - parentPosition.x,
					sourceBone.position.y - parentPosition.y,
					sourceBone.position.z - parentPosition.z
				};
			}
			node.localMatrix = MakeAffineMatrix(
				node.transform.scale,
				node.transform.rotate,
				node.transform.translate
			);
			node.children.reserve(boneChildren[boneIndex].size());
			for (const size_t childIndex : boneChildren[boneIndex]) {
				node.children.push_back(buildBoneNode(childIndex));
			}
			return node;
		};

		modelData.rootNode.children.reserve(rootBones.size());
		for (const size_t rootBoneIndex : rootBones) {
			modelData.rootNode.children.push_back(buildBoneNode(rootBoneIndex));
		}

		modelData.vertices.reserve(pmxData.vertices.size());
		for (const PmxLoader::Vertex& sourceVertex : pmxData.vertices) {
			modelData.vertices.push_back({
				{
					sourceVertex.position.x,
					sourceVertex.position.y,
					sourceVertex.position.z,
					1.0f
				},
				sourceVertex.texcoord,
				sourceVertex.normal
			});
		}
		modelData.indices = std::move(pmxData.indices);
		modelData.materials.reserve(pmxData.materials.size());
		modelData.subMeshes.reserve(pmxData.materials.size());
		uint32_t firstIndex = 0;
		for (size_t materialIndex = 0;
			materialIndex < pmxData.materials.size();
			++materialIndex) {
			const PmxLoader::Material& sourceMaterial =
				pmxData.materials[materialIndex];
			MaterialSlot material{};
			material.name = sourceMaterial.name;
			material.baseColor = sourceMaterial.baseColor;
			if (!sourceMaterial.texturePath.empty()) {
				const std::filesystem::path texturePath =
					StringUtility::ToPath(sourceMaterial.texturePath);
				material.textureFilePath = StringUtility::ToUtf8(
					(texturePath.is_absolute()
						? texturePath
						: modelDirectory / texturePath).lexically_normal()
				);
			}
			modelData.materials.push_back(std::move(material));

			SubMesh subMesh{};
			subMesh.name = sourceMaterial.name;
			subMesh.firstIndex = firstIndex;
			subMesh.indexCount = sourceMaterial.indexCount;
			subMesh.materialIndex = static_cast<uint32_t>(materialIndex);
			modelData.subMeshes.push_back(std::move(subMesh));
			firstIndex += sourceMaterial.indexCount;
		}
		candidate.model = std::move(modelData);
		return true;
	}

	Assimp::Importer importer;
	if (!importer.IsExtensionSupported(extension.c_str())) {
		diagnostics.push_back(
			"Assimp does not support this model format in the current build: " +
			filePath + "\n"
		);
		return false;
	}
	importer.SetIOHandler(new AssimpUnicodeIOSystem(modelDirectory));
	const aiScene* scene = importer.ReadFile(
		filePath.c_str(),
		aiProcess_FlipWindingOrder |
		aiProcess_FlipUVs |
		aiProcess_Triangulate |
		aiProcess_GenSmoothNormals
	);
	if (!scene) {
		diagnostics.push_back(
			"Failed to load model: " + filePath +
			"\nAssimp: " + importer.GetErrorString() + "\n"
		);
		return false;
	}
	if (!scene->HasMeshes() || !scene->mRootNode) {
		diagnostics.push_back("Model contains no mesh or root node: " + filePath + "\n");
		return false;
	}

	const bool bakeNodeTransforms = extension == ".glb" && isAlleyResource;
	const auto rejectStaticModel = [&](const std::string& reason) {
		diagnostics.push_back("Cannot load alley static GLB: " + filePath + " (" + reason + ")\n");
		return false;
	};
	std::vector<StaticMeshInstance> instances;
	if (bakeNodeTransforms) {
		if (scene->HasAnimations()) {
			return rejectStaticModel("animations are unsupported");
		}
		for (uint32_t i = 0; i < scene->mNumMeshes; ++i) {
			const aiMesh* mesh = scene->mMeshes[i];
			if (!mesh || mesh->HasBones() || mesh->mNumAnimMeshes != 0) {
				return rejectStaticModel("invalid mesh, bones, or morph targets");
			}
		}

		// Assimpの列ベクトル規約で親から合成する。同じmeshの参照も配置ごとに持つ。
		const auto collect = [&](const auto& visit, const aiNode* node,
			const aiMatrix4x4& parent) -> bool {
			if (!node) {
				return rejectStaticModel("invalid node reference");
			}
			const aiMatrix4x4 transform = parent * node->mTransformation;
			const ai_real entries[] = {
				transform.a1, transform.a2, transform.a3, transform.a4,
				transform.b1, transform.b2, transform.b3, transform.b4,
				transform.c1, transform.c2, transform.c3, transform.c4,
				transform.d1, transform.d2, transform.d3, transform.d4
			};
			if (!std::all_of(std::begin(entries), std::end(entries),
				[](ai_real value) { return std::isfinite(value); })) {
				return rejectStaticModel("non-finite node transform");
			}
			if (node->mNumMeshes != 0) {
				if (!node->mMeshes) {
					return rejectStaticModel("missing node mesh references");
				}
				aiMatrix3x3 normalTransform(transform);
				const ai_real determinant = normalTransform.Determinant();
				if (!std::isfinite(determinant) || determinant == 0) {
					return rejectStaticModel("singular node transform");
				}
				normalTransform.Inverse().Transpose();
				for (uint32_t i = 0; i < node->mNumMeshes; ++i) {
					const uint32_t index = node->mMeshes[i];
					if (index >= scene->mNumMeshes) {
						return rejectStaticModel("invalid node mesh index");
					}
					instances.push_back({index, transform, normalTransform, determinant < 0});
				}
			}
			if (node->mNumChildren != 0 && !node->mChildren) {
				return rejectStaticModel("missing child node references");
			}
			for (uint32_t i = 0; i < node->mNumChildren; ++i) {
				if (!visit(visit, node->mChildren[i], transform)) {
					return false;
				}
			}
			return true;
		};
		if (!collect(collect, scene->mRootNode, aiMatrix4x4{})) {
			return false;
		}
	}

	ModelData modelData; //構築するModelData
	if (bakeNodeTransforms) {
		// 頂点にルートを含めたため、Object3d側での再適用を防ぐ。
		modelData.rootNode.localMatrix = MakeIdentity4x4();
	} else {
		modelData.rootNode = ReadNode(scene->mRootNode);
	}
	modelData.materials.reserve(scene->mNumMaterials);
	for (uint32_t materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex) {
		aiMaterial* sourceMaterial = scene->mMaterials[materialIndex];
		MaterialSlot material{};
		aiString materialName;
		if (sourceMaterial->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS) {
			material.name = materialName.C_Str();
		}
		if (material.name.empty()) {
			material.name = "Material " + std::to_string(materialIndex + 1);
		}
		aiColor4D diffuseColor{};
		if (sourceMaterial->Get(AI_MATKEY_COLOR_DIFFUSE, diffuseColor) == AI_SUCCESS) {
			material.baseColor = {
				diffuseColor.r, diffuseColor.g, diffuseColor.b, diffuseColor.a
			};
		}

		const aiTextureType textureTypes[] = {
			aiTextureType_BASE_COLOR,
			aiTextureType_DIFFUSE
		};
		for (aiTextureType textureType : textureTypes) {
			if (sourceMaterial->GetTextureCount(textureType) == 0) {
				continue;
			}
			aiString textureFilePath;
			sourceMaterial->GetTexture(textureType, 0, &textureFilePath);
			const std::string texturePath = textureFilePath.C_Str();
			if (!texturePath.empty() && texturePath.front() == '*') {
				const uint32_t textureIndex = static_cast<uint32_t>(
					std::strtoul(texturePath.c_str() + 1, nullptr, 10)
				);
				if (textureIndex < scene->mNumTextures && scene->mTextures && scene->mTextures[textureIndex]) {
					const aiTexture* embeddedTexture = scene->mTextures[textureIndex];
					std::string textureKey =
						filePath + "#embedded" + std::to_string(textureIndex);
					if (embeddedTexture->mHeight == 0) {
						const auto* data = reinterpret_cast<const uint8_t*>(embeddedTexture->pcData);
						const size_t size = embeddedTexture->mWidth;
						// DIFFUSEもBASE_COLORの代わりに選ばれた場合はモデルの色として扱う。
						const bool expandGray = (extension == ".glb" || extension == ".gltf") &&
							IsGrayscalePng(data, size);
						if (expandGray) textureKey += "#base-color-rgb";
						auto& sources = candidate.embeddedTextures;
						const auto found = std::find_if(sources.begin(), sources.end(),
							[&textureKey](const EmbeddedTextureSource& source) { return source.key == textureKey; });
						const size_t sourceIndex = static_cast<size_t>(found - sources.begin());
						if (found == sources.end()) {
							EmbeddedTextureSource source;
							source.key = std::move(textureKey);
							source.formatHint = embeddedTexture->achFormatHint;
							std::transform(source.formatHint.begin(), source.formatHint.end(), source.formatHint.begin(),
								[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							source.expandGrayscaleBaseColor = expandGray;
							// Importer破棄後も同期登録で使うため、画像bytesを所有する。
							if (data && size) source.bytes.assign(data, data + size);
							sources.push_back(std::move(source));
						}
						candidate.embeddedBindings.push_back({materialIndex, sourceIndex});
					}
				}
			} else {
				material.textureFilePath = StringUtility::ToUtf8(
					(modelDirectory / StringUtility::ToPath(texturePath))
						.lexically_normal()
				);
			}
			break;
		}
		modelData.materials.push_back(std::move(material));
	}
	if (modelData.materials.empty()) {
		modelData.materials.push_back({});
	}

	const size_t meshCount = bakeNodeTransforms ? instances.size() : scene->mNumMeshes;
	for (size_t i = 0; i < meshCount; ++i) {
		const StaticMeshInstance* instance = bakeNodeTransforms ? &instances[i] : nullptr;
		const size_t meshIndex = instance ? instance->meshIndex : i;
		aiMesh* mesh = scene->mMeshes[meshIndex];
		if (instance && (!mesh->HasPositions() || !mesh->HasFaces())) {
			return rejectStaticModel("referenced mesh has no drawable geometry");
		}
		const aiVector3D* normals = mesh->HasNormals()
			? mesh->mNormals
			: nullptr;
		const aiVector3D* textureCoordinates = mesh->HasTextureCoords(0)
			? mesh->mTextureCoords[0]
			: nullptr;
		modelData.hasSkinning =
			modelData.hasSkinning || mesh->HasBones();

		// D3D12のViewが保持するバイト数に収まることを追加前に確認する。
		if (instance && mesh->mNumVertices >
			(std::numeric_limits<UINT>::max)() / sizeof(VertexData) - modelData.vertices.size()) {
			return rejectStaticModel("vertex buffer size exceeds D3D12 view capacity");
		}
		const uint32_t vertexOffset =
			static_cast<uint32_t>(modelData.vertices.size());
		modelData.vertices.resize(
			modelData.vertices.size() + mesh->mNumVertices
		);
		for (uint32_t vertexIndex = 0;
			vertexIndex < mesh->mNumVertices;
			++vertexIndex) {
			aiVector3D position = mesh->mVertices[vertexIndex];
			aiVector3D normal = normals
				? normals[vertexIndex]
				: aiVector3D{ 0.0f, 1.0f, 0.0f };
			if (instance) {
				position = instance->transform * position;
				normal = instance->normalTransform * normal;
				const ai_real length = normal.Length();
				if (!IsFiniteVector(position) || !std::isfinite(length) || length == 0) {
					return rejectStaticModel("invalid transformed vertex or normal");
				}
				normal /= length;
			}
			// glTFではUVが任意なので、未設定メッシュは原点UVとして扱う。
			const aiVector3D texcoord = textureCoordinates
				? textureCoordinates[vertexIndex]
				: aiVector3D{};
			VertexData& vertex =
				modelData.vertices[vertexOffset + vertexIndex];
			vertex.position = {
				-position.x,
				position.y,
				position.z,
				1.0f
			};
			vertex.normal = {
				-normal.x,
				normal.y,
				normal.z
			};
			vertex.texcoord = { texcoord.x, texcoord.y };
		}

		SubMesh subMesh{};
		subMesh.name = mesh->mName.C_Str();
		subMesh.firstIndex = static_cast<uint32_t>(modelData.indices.size());
		subMesh.materialIndex = mesh->mMaterialIndex < modelData.materials.size()
			? mesh->mMaterialIndex
			: 0;
		for (uint32_t faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
			aiFace& face = mesh->mFaces[faceIndex];
			if (face.mNumIndices != 3 || (instance && !face.mIndices)) {
				if (instance) {
					return rejectStaticModel("non-triangular face");
				}
				diagnostics.push_back(
					"Skipped a non-triangulated face while loading model: " +
					filePath + "\n"
				);
				continue;
			}
			if (instance && modelData.indices.size() >
				(std::numeric_limits<UINT>::max)() / sizeof(uint32_t) - 3) {
				return rejectStaticModel("index buffer size exceeds D3D12 view capacity");
			}
			for (uint32_t element = 0; element < face.mNumIndices; ++element) {
				// 既存のX反転に対応する面順に、ノードの鏡映分だけ補正を加える。
				const uint32_t corner = instance && instance->reverseWinding
					? 2 - element
					: element;
				const uint32_t index = face.mIndices[corner];
				if (instance && index >= mesh->mNumVertices) {
					return rejectStaticModel("invalid face vertex index");
				}
				modelData.indices.push_back(vertexOffset + index);
			}
		}
		subMesh.indexCount = static_cast<uint32_t>(modelData.indices.size()) -
			subMesh.firstIndex;
		modelData.subMeshes.push_back(std::move(subMesh));

		for (uint32_t boneIndex = 0;
			boneIndex < mesh->mNumBones;
			++boneIndex) {
			const aiBone* bone = mesh->mBones[boneIndex];
			const std::string jointName = bone->mName.C_Str();
			JointWeightData& jointWeightData =
				modelData.skinClusterData[jointName];

			jointWeightData.inverseBindPoseMatrix =
				ConvertAssimpMatrix(bone->mOffsetMatrix);

			for (uint32_t weightIndex = 0;
				weightIndex < bone->mNumWeights;
				++weightIndex) {
				const aiVertexWeight& weight =
					bone->mWeights[weightIndex];
				jointWeightData.vertexWeights.push_back({
					weight.mWeight,
					vertexOffset + weight.mVertexId
				});
			}
		}
	}

	if (modelData.vertices.empty() || modelData.indices.empty()) {
		diagnostics.push_back("Model contains no drawable geometry: " + filePath + "\n");
		return false;
	}
	candidate.model = std::move(modelData);
	return true;
}

void Model::CreateVertexResource() {
	vertexResource = *&modelCommon->GetDxCommon()->CreateBufferResource(sizeof(VertexData) * modelData.vertices.size());

	vertexBufferView.BufferLocation = vertexResource->GetGPUVirtualAddress();
	vertexBufferView.SizeInBytes = UINT(sizeof(VertexData) * modelData.vertices.size());
	vertexBufferView.StrideInBytes = sizeof(VertexData);

	vertexResource->Map(0, nullptr, reinterpret_cast<void**>(&vertexData));
	std::memcpy(vertexData, modelData.vertices.data(), sizeof(VertexData) * modelData.vertices.size());
}

void Model::CreateIndexResource() {
	indexResource = *&modelCommon->GetDxCommon()->CreateBufferResource(
		sizeof(uint32_t) * modelData.indices.size()
	);
	indexBufferView.BufferLocation =
		indexResource->GetGPUVirtualAddress();
	indexBufferView.SizeInBytes = static_cast<UINT>(
		sizeof(uint32_t) * modelData.indices.size()
	);
	indexBufferView.Format = DXGI_FORMAT_R32_UINT;

	indexResource->Map(
		0,
		nullptr,
		reinterpret_cast<void**>(&indexData)
	);
	std::memcpy(
		indexData,
		modelData.indices.data(),
		sizeof(uint32_t) * modelData.indices.size()
	);
}

Model::Material Model::MakeMaterial(const MaterialSlot& slot) {
	Material material{};
	material.color = slot.baseColor;
	material.enableLighting = true;
	material.uvTransform = MakeIdentity4x4();
	material.emissiveColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
	material.shininess = 40.0f;
	material.dissolveEdgeWidth = 0.08f;
	material.dissolveNoiseScale = 6.0f;
	return material;
}

void Model::CreateMaterialResource() {
	materialResources.resize(modelData.materials.size());
	materialDataList.resize(modelData.materials.size());
	for (size_t materialIndex = 0; materialIndex < modelData.materials.size(); ++materialIndex) {
		materialResources[materialIndex] = *&modelCommon->GetDxCommon()
			->CreateBufferResource(sizeof(Material));
		materialResources[materialIndex]->Map(
			0, nullptr, reinterpret_cast<void**>(&materialDataList[materialIndex])
		);
		*materialDataList[materialIndex] = MakeMaterial(modelData.materials[materialIndex]);
	}
}

struct ModelBuild::Impl {
	enum class Stage { VertexBuffer, IndexBuffer, VertexCopy, IndexCopy, Materials, Ready, Failed, Taken };
	std::unique_ptr<Model> model;
	Stage stage = Stage::VertexBuffer;
	size_t copied = 0;
	size_t material = 0;
	Model::Material materialValue{};
};

ModelBuild::ModelBuild() : impl_(std::make_unique<Impl>()) {}
ModelBuild::~ModelBuild() = default;

bool Model::TryBeginPrepared(ModelCommon* common, PreparedModelData& input,
	std::unique_ptr<ModelBuild>& out, std::string& diagnostic) {
	const auto fail = [&](const char* reason) { diagnostic = reason; return false; };
	diagnostic.clear();
	const auto& source = input.model;
	constexpr auto maxViewBytes = (std::numeric_limits<UINT>::max)();
	if (!common || !common->GetDxCommon() || source.vertices.empty() || source.indices.empty() ||
		source.materials.empty() || source.subMeshes.empty() ||
		source.vertices.size() > maxViewBytes / sizeof(VertexData) ||
		source.indices.size() > maxViewBytes / sizeof(uint32_t) ||
		source.materials.size() > maxViewBytes)
		return fail("Prepared model dimensions or host are invalid.");
	for (const auto index : source.indices)
		if (index >= source.vertices.size()) return fail("Prepared model index exceeds the vertex buffer.");
	for (const auto& mesh : source.subMeshes)
		if (mesh.materialIndex >= source.materials.size() || mesh.firstIndex > source.indices.size() ||
			mesh.indexCount > source.indices.size() - mesh.firstIndex || mesh.indexCount % 3 != 0)
			return fail("Prepared submesh index range or material is invalid.");
	for (const auto& joint : source.skinClusterData) {
		for (const auto& weight : joint.second.vertexWeights)
			if (weight.vertexIndex >= source.vertices.size() || !std::isfinite(weight.weight))
				return fail("Prepared skin weight is invalid.");
	}
	auto& textures = *TextureManager::GetInstance();
	for (const auto& material : source.materials)
		if (material.textureFilePath.empty() || !textures.HasTexture(material.textureFilePath))
			return fail("A prepared model material texture has not been published.");
	try {
		auto candidate = std::unique_ptr<ModelBuild>(new ModelBuild());
		auto model = std::make_unique<Model>();
		model->modelCommon = common;
		model->materialResources.resize(source.materials.size());
		model->materialDataList.resize(source.materials.size());
		model->preparedResources_.resize(source.materials.size() + 2);
		// 可失敗の確保後にだけ入力を消費し、skin/animationを含む所有を引き継ぐ。
		static_assert(noexcept(model->modelData = std::move(input.model)));
		static_assert(noexcept(model->animations_ = std::move(input.animations)));
		model->modelData = std::move(input.model);
		model->animations_ = std::move(input.animations);
		input.embeddedTextures.clear();
		input.embeddedBindings.clear();
		candidate->impl_->model = std::move(model);
		out = std::move(candidate);
		return true;
	} catch (const std::exception&) {
		return fail("Failed to allocate a prepared model build.");
	}
}

PreparedBuildStep Model::StepPrepared(ModelBuild& build, const ModelBuildBudget& budget,
	std::string& diagnostic) {
	using Stage = ModelBuild::Impl::Stage;
	using Step = PreparedBuildStep;
	auto& state = *build.impl_;
	diagnostic.clear();
	if (state.stage == Stage::Ready) return Step::Ready;
	if (!state.model) {
		diagnostic = "The prepared model build has failed or was already taken.";
		return Step::Failed;
	}
	auto& model = *state.model;
	auto& dx = *model.modelCommon->GetDxCommon();
	auto resources = budget.maxResources;
	auto bytes = budget.maxCopyBytes;
	const auto fail = [&]() {
		state.stage = Stage::Failed;
		state.model.reset();
		diagnostic = "Failed to create or map a prepared model upload buffer.";
		return Step::Failed;
	};
	const auto allocate = [&](size_t slot, size_t size, void*& mapped) {
		auto& resource = model.preparedResources_[slot];
		D3D12_RANGE readRange{0, 0};
		return dx.TryCreateBufferResource(size, resource) && resource &&
			SUCCEEDED(resource->Map(0, &readRange, &mapped)) && mapped;
	};
	const auto copy = [&](void* destination, const void* source, size_t size) {
		const auto count = static_cast<size_t>((std::min)(bytes, static_cast<uint64_t>(size - state.copied)));
		std::memcpy(static_cast<std::byte*>(destination) + state.copied,
			static_cast<const std::byte*>(source) + state.copied, count);
		bytes -= count;
		state.copied += count;
		if (state.copied != size) return false;
		state.copied = 0;
		return true;
	};
	const size_t vertexBytes = model.modelData.vertices.size() * sizeof(VertexData);
	const size_t indexBytes = model.modelData.indices.size() * sizeof(uint32_t);
	for (;;) {
		void* mapped = nullptr;
		switch (state.stage) {
		case Stage::VertexBuffer:
			if (!resources) return Step::Pending;
			if (!allocate(0, vertexBytes, mapped)) return fail();
			--resources;
			model.vertexResource = model.preparedResources_[0].Get();
			model.vertexData = static_cast<VertexData*>(mapped);
			model.vertexBufferView = {model.vertexResource->GetGPUVirtualAddress(),
				static_cast<UINT>(vertexBytes), sizeof(VertexData)};
			state.stage = Stage::IndexBuffer;
			break;
		case Stage::IndexBuffer:
			if (!resources) return Step::Pending;
			if (!allocate(1, indexBytes, mapped)) return fail();
			--resources;
			model.indexResource = model.preparedResources_[1].Get();
			model.indexData = static_cast<uint32_t*>(mapped);
			model.indexBufferView = {model.indexResource->GetGPUVirtualAddress(),
				static_cast<UINT>(indexBytes), DXGI_FORMAT_R32_UINT};
			state.stage = Stage::VertexCopy;
			break;
		case Stage::VertexCopy:
			if (!bytes || !copy(model.vertexData, model.modelData.vertices.data(), vertexBytes)) return Step::Pending;
			state.stage = Stage::IndexCopy;
			break;
		case Stage::IndexCopy:
			if (!bytes || !copy(model.indexData, model.modelData.indices.data(), indexBytes)) return Step::Pending;
			state.stage = Stage::Materials;
			break;
		case Stage::Materials: {
			if (state.material == model.modelData.materials.size()) {
				model.preparedReady_ = true;
				state.stage = Stage::Ready;
				return Step::Ready;
			}
			if (!bytes) return Step::Pending;
			const auto i = state.material;
			if (!model.materialResources[i]) {
				if (!resources) return Step::Pending;
				if (!allocate(i + 2, sizeof(Material), mapped)) return fail();
				--resources;
				model.materialResources[i] = model.preparedResources_[i + 2].Get();
				model.materialDataList[i] = static_cast<Material*>(mapped);
				state.materialValue = MakeMaterial(model.modelData.materials[i]);
			}
			if (!copy(model.materialDataList[i], &state.materialValue, sizeof(Material))) return Step::Pending;
			++state.material;
			break;
		}
		default:
			return Step::Failed;
		}
	}
}

bool Model::TryTakePrepared(ModelBuild& build, std::unique_ptr<Model>& out) noexcept {
	auto& state = *build.impl_;
	if (state.stage != ModelBuild::Impl::Stage::Ready || !state.model) return false;
	out = std::move(state.model);
	state.stage = ModelBuild::Impl::Stage::Taken;
	return true;
}

Model::Node Model::ReadNode(aiNode* aiNode)
{
	Node result;
	aiVector3D scale;
	aiQuaternion rotate;
	aiVector3D translate;
	aiNode->mTransformation.Decompose(scale, rotate, translate);

	result.transform.scale = { scale.x, scale.y, scale.z };
	result.transform.rotate = Normalize({
		rotate.x,
		-rotate.y,
		-rotate.z,
		rotate.w
	});
	result.transform.translate = {
		-translate.x,
		translate.y,
		translate.z
	};
	result.localMatrix = MakeAffineMatrix(
		result.transform.scale,
		result.transform.rotate,
		result.transform.translate
	);
	result.name = aiNode->mName.C_Str();
	result.children.resize(aiNode->mNumChildren);
	for (uint32_t childIndex = 0; childIndex < aiNode->mNumChildren; ++childIndex) {
		result.children[childIndex] = ReadNode(aiNode->mChildren[childIndex]);
	}
	return result;
}
