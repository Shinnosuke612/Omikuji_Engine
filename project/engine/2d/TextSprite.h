// 役割: Runtime生成Text textureを2D quadとして描画する。
#pragma once
#include "../scene/SceneRuntimePreparation.h"

#include <string>
#include <utility>

#include <d3d12.h>
#include <wrl.h>

#include "../math/Matrix4x4.h"
#include "../math/Vector2.h"
#include "../math/Vector3.h"
#include "../math/Vector4.h"

class DirectXCommon;

class TextSprite {
public:
	enum class OutputTarget { Display, SceneHdr };

	// main専用。公開済み資源を借用し、Readyまで描画しない。
	bool BeginPreparedInitialize(DirectXCommon& dx, const std::string& publishedTextureKey,
		std::string& diagnostic);
	RuntimePrepareStep StepPreparedInitialize(SceneRuntimePrepareBudget& remaining, std::string& diagnostic);
	void Initialize(DirectXCommon* dxCommon, std::string textureKey);
	void SetTextureKey(std::string textureKey) { textureKey_ = std::move(textureKey); }
	void Update(
		const Vector2& position,
		float rotation,
		const Vector2& size,
		const Vector2& pivot,
		const Vector4& color,
		uint32_t canvasWidth,
		uint32_t canvasHeight
	);
	// View別snapshotをBindする。Bufferは既存のFrame末Fenceまで保持する。
	void Draw(OutputTarget target, bool editorView = false) const;
	// [0,1] quadからclipまでの完全な行列を設定する。viewport寸法を保存値へ混ぜない。
	void UpdateUI(const Matrix4x4& unitQuadToClip, const Vector4& color);

private:
	struct VertexData {
		Vector4 position;
		Vector2 texcoord;
		Vector3 normal;
	};
	struct Material {
		Vector4 color;
		int32_t enableLighting;
		float padding[3];
		Matrix4x4 uvTransform;
	};
	struct TransformationMatrix {
		Matrix4x4 WVP;
		Matrix4x4 World;
	};


	struct ViewDrawBuffers {
		Microsoft::WRL::ComPtr<ID3D12Resource> vertices, material, transform;
	};
	// texture所有は共有。Draw時の頂点/定数だけを通常ViewとUI編集Viewで分ける。
	mutable ViewDrawBuffers viewDrawBuffers_[2];
	void CreateResources();

	DirectXCommon* dxCommon_ = nullptr;
	std::string textureKey_;
	Microsoft::WRL::ComPtr<ID3D12Resource> vertexResource_;
	Microsoft::WRL::ComPtr<ID3D12Resource> indexResource_;
	Microsoft::WRL::ComPtr<ID3D12Resource> materialResource_;
	Microsoft::WRL::ComPtr<ID3D12Resource> transformationResource_;
	VertexData* vertexData_ = nullptr;
	int preparedStage_ = 0;
	uint32_t* indexData_ = nullptr;
	Material* materialData_ = nullptr;
	TransformationMatrix* transformationData_ = nullptr;
	D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{};
	D3D12_INDEX_BUFFER_VIEW indexBufferView_{};
};
