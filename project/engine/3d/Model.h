// 役割: メッシュ、材質、Skeleton、Animationを持つモデルリソースを定義する。
#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cassert>
#include <map>
#include "../math/Vector2.h"
#include "../math/Vector3.h"
#include "../math/Vector4.h"
#include "../math/Matrix4x4.h"
#include "../math/Transform.h"
#include "Animation.h"
#include <d3d12.h> 
class ModelCommon;
class aiNode;
class Model{
public:
	struct Node {
		QuaternionTransform transform;
		Matrix4x4 localMatrix;
		std::string name;
		std::vector<Node> children;
	};

	struct VertexWeightData {
		float weight = 0.0f;
		uint32_t vertexIndex = 0;
	};

	struct JointWeightData {
		Matrix4x4 inverseBindPoseMatrix = MakeIdentity4x4();
		std::vector<VertexWeightData> vertexWeights;
	};

	using SkinClusterData = std::map<std::string, JointWeightData>;

	struct VertexData{
		Vector4 position;
		Vector2 texcoord;
		Vector3 normal;
	};

	// モデル内で共有する標準材質。Entityごとの上書きはObject3dが所有する。
	struct MaterialSlot {
		std::string name;
		Vector4 baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		std::string textureFilePath;
	};

	// 共通の頂点・Indexバッファに対する描画範囲。
	struct SubMesh {
		std::string name;
		uint32_t firstIndex = 0;
		uint32_t indexCount = 0;
		uint32_t materialIndex = 0;
	};

private://インナークラス
	struct ModelData{
		std::vector<VertexData> vertices;
		std::vector<uint32_t> indices;
		std::vector<MaterialSlot> materials;
		std::vector<SubMesh> subMeshes;
		Node rootNode;
		SkinClusterData skinClusterData;
		bool hasSkinning = false;
	};
	//マテリアルデータ
	struct Material {
		Vector4 color;
		int32_t enableLighting;
		float emissiveIntensity;
		float padding[2];
		Matrix4x4 uvTransform;
		Vector4 emissiveColor;
		float shininess;
		float dissolveAmount;
		float dissolveEdgeWidth;
		float dissolveNoiseScale;
	};
private://非公開メンバ関数
	// 内蔵PMXローダーまたはAssimp経由で基本モデルを読み取る。
	bool LoadModelFile(
		const std::filesystem::path& modelFilePath,
		ModelData& result
	);

	//頂点リソース作成関数
	void CreateVertexResource();
	void CreateIndexResource();
	//マテリアルリソース作成関数
	void CreateMaterialResource();

	Node ReadNode(aiNode* aiNode);
public://公開メンバ関数
	//初期化
	bool Initialize(
		ModelCommon* modelCommon,
		const std::filesystem::path& modelFilePath
	);
	//描画
	void Draw(const D3D12_VERTEX_BUFFER_VIEW* influenceBufferView = nullptr);
	void DrawForShadow(const D3D12_VERTEX_BUFFER_VIEW* influenceBufferView = nullptr);
	void DrawWithMaterial(ID3D12Resource* materialOverride, const D3D12_VERTEX_BUFFER_VIEW* influenceBufferView = nullptr);
	void DrawWithMaterialAndTexture(
		ID3D12Resource* materialOverride,
		D3D12_GPU_DESCRIPTOR_HANDLE textureOverride,
		const D3D12_VERTEX_BUFFER_VIEW* influenceBufferView = nullptr
	);
	void DrawWithMaterialSlots(
		const std::vector<ID3D12Resource*>& materialOverrides,
		const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& textureOverrides,
		const D3D12_VERTEX_BUFFER_VIEW* influenceBufferView = nullptr
	);
	void DrawWithVertexBufferAndMaterial(
		const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView,
		ID3D12Resource* materialOverride,
		D3D12_GPU_DESCRIPTOR_HANDLE textureOverride = {}
	);
	void DrawWithVertexBufferAndMaterialSlots(
		const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView,
		const std::vector<ID3D12Resource*>& materialOverrides,
		const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE>& textureOverrides
	);
	void DrawForShadowWithVertexBuffer(const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView);

	// getter
	const Matrix4x4& GetRootNodeLocalMatrix() const { return modelData.rootNode.localMatrix; }
	const Node& GetRootNode() const { return modelData.rootNode; }
	const std::vector<Animation>& GetAnimations() const { return animations_; }
	const std::vector<MaterialSlot>& GetMaterialSlots() const {
		return modelData.materials;
	}
	const std::vector<SubMesh>& GetSubMeshes() const {
		return modelData.subMeshes;
	}
	bool HasAnimation() const { return !animations_.empty(); }
	bool HasSkinning() const { return modelData.hasSkinning; }
	uint32_t GetVertexCount() const {
		return static_cast<uint32_t>(modelData.vertices.size());
	}
	const std::vector<VertexData>& GetVertices() const { return modelData.vertices; }
	bool GetLocalBounds(Vector3& boundsMin, Vector3& boundsMax) const;
	const SkinClusterData& GetSkinClusterData() const {
		return modelData.skinClusterData;
	}
	ID3D12Resource* GetVertexResource() const { return vertexResource; }
	const D3D12_VERTEX_BUFFER_VIEW& GetVertexBufferView() const {
		return vertexBufferView;
	}
private:
	//ModelCommonのポインタ
	ModelCommon* modelCommon;
	//objファイルのデータ
	ModelData modelData;
	//バッファリソース
	ID3D12Resource* vertexResource;
	//バッファリソース内のデータを指すポインタ
	VertexData* vertexData = nullptr;
	//バッファリソースの使い道を補足するバッファビュー
	D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
	ID3D12Resource* indexResource = nullptr;
	uint32_t* indexData = nullptr;
	D3D12_INDEX_BUFFER_VIEW indexBufferView{};

	//バッファリソース
	std::vector<ID3D12Resource*> materialResources;
	std::vector<Material*> materialDataList;
	std::vector<Animation> animations_;
};

