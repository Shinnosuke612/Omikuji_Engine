// 役割: メッシュ、材質、Skeleton、Animationを持つモデルリソースを定義する。
#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <wrl.h>
#include "../math/Vector2.h"
#include "../math/Vector3.h"
#include "../math/Vector4.h"
#include "../math/Matrix4x4.h"
#include "../math/Transform.h"
#include "Animation.h"
#include <d3d12.h> 
class ModelCommon;
class aiNode;
class ModelManager;
class ModelBuild;
enum class PreparedBuildStep { Pending, Ready, Failed };
struct ModelBuildBudget {
	uint64_t maxCopyBytes = 0;
	uint32_t maxResources = 0;
};
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

	struct ModelData{
		std::vector<VertexData> vertices;
		std::vector<uint32_t> indices;
		std::vector<MaterialSlot> materials;
		std::vector<SubMesh> subMeshes;
		Node rootNode;
		SkinClusterData skinClusterData;
		bool hasSkinning = false;
	};

	struct EmbeddedTextureSource {
		std::string key;
		std::string formatHint;
		std::vector<uint8_t> bytes;
		bool expandGrayscaleBaseColor = false;
	};
	struct EmbeddedMaterialBinding {
		size_t materialIndex;
		size_t sourceIndex;
	};
	// Importerの寿命から独立したCPUの解析結果。画像の登録は同期入口で行う。
	struct PreparedModelData {
		ModelData model;
		std::vector<Animation> animations;
		std::vector<EmbeddedTextureSource> embeddedTextures;
		std::vector<EmbeddedMaterialBinding> embeddedBindings;
	};

	// 解決済みパスを借用する。診断を更新し、成功時だけoutを置き換える。
	static bool TryPrepareModelData(const std::filesystem::path& resolvedModelPath,
		bool isAlleyResource, PreparedModelData& out, std::vector<std::string>& diagnostics);
	// main専用。材質keyは公開済みであること。成功したBeginだけinputを消費する。
	static bool TryBeginPrepared(ModelCommon*, PreparedModelData& input,
		std::unique_ptr<ModelBuild>& out, std::string& diagnostic);
	// 一回分の予算で進める。部分資源はbuildが所有し、GPU提出/待機は行わない。
	static PreparedBuildStep StepPrepared(ModelBuild&, const ModelBuildBudget&, std::string& diagnostic);
	// Ready後に一回だけ取得する。拒否時out不変。
	static bool TryTakePrepared(ModelBuild&, std::unique_ptr<Model>& out) noexcept;

private:
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
	static bool TryParseModelFile(
		const std::filesystem::path& modelFilePath,
		bool isAlleyResource, PreparedModelData& candidate,
		std::vector<std::string>& diagnostics
	);

	//頂点リソース作成関数
	void CreateVertexResource();
	void CreateIndexResource();
	//マテリアルリソース作成関数
	void CreateMaterialResource();
	static Material MakeMaterial(const MaterialSlot&);

	static Node ReadNode(aiNode* aiNode);
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
	// 準備経路の資源所有。既存描画には同じraw viewを渡す。
	std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> preparedResources_;
	bool preparedReady_ = false;
	friend class ModelManager;
	friend class ModelBuild;
};

// main専用。取消時はその時点の未公開Modelと資源を解放する。
class ModelBuild final {
public:
	~ModelBuild();
	ModelBuild(const ModelBuild&) = delete;
	ModelBuild& operator=(const ModelBuild&) = delete;
private:
	ModelBuild();
	struct Impl;
	std::unique_ptr<Impl> impl_;
	friend class Model;
};

