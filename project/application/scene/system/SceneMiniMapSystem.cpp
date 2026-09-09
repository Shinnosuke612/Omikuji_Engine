// 役割: 固定設定のHUDミニマップ描画を実装する。
#include "SceneMiniMapSystem.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

#include "../../../engine/2d/TextureManager.h"
#include "../../../engine/3d/Camera.h"
#include "../../../engine/3d/SrvManager.h"
#include "../../../engine/base/DirectXCommon.h"
#include "../../../engine/base/RenderFormats.h"
#include "../../../engine/base/SceneRenderTarget.h"
#include "../../../engine/math/Math.h"
#include "../../../engine/scene/SceneDocument.h"
#include "../../../engine/scene/SceneEntityQuery.h"
#include "../../../engine/scene/SceneTransformResolver.h"

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::HasComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;
	using SceneTransformResolver::ResolveScene3DTransform;

	constexpr uint32_t kMiniMapTextureSize = 256;
	constexpr uint32_t kMiniMapCircleTextureSize = 32;
	constexpr uint32_t kMaxMiniMapQuadCount = 96;
	constexpr uint32_t kConstantBufferAlignment = 256;

	/// <summary>
	/// 指定サイズをAlignment境界へ切り上げる。
	/// </summary>
	constexpr uint32_t AlignTo(uint32_t value, uint32_t alignment) {
		return (value + alignment - 1u) & ~(alignment - 1u);
	}

	class MiniMapQuadPipeline {
	public:
		/// <summary>
		/// HUD用クアッド描画Pipelineを初期化する。
		/// </summary>
		void Initialize(DirectXCommon* dxCommon) {
			if (initialized_) {
				return;
			}
			dxCommon_ = dxCommon;
			CreateRootSignature();
			CreatePipeline();
			initialized_ = true;
		}

		/// <summary>
		/// HUD用クアッド描画PipelineをCommandListへ設定する。
		/// </summary>
		void SetCommonRenderState() const {
			dxCommon_->GetCommandList()->SetGraphicsRootSignature(
				rootSignature_.Get()
			);
			dxCommon_->GetCommandList()->SetPipelineState(
				pipelineState_.Get()
			);
			dxCommon_->GetCommandList()->IASetPrimitiveTopology(
				D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
			);
		}

	private:
		/// <summary>
		/// Sprite Shader互換のRootSignatureを作成する。
		/// </summary>
		void CreateRootSignature() {
			D3D12_DESCRIPTOR_RANGE descriptorRange{}; // t0のTexture SRV
			descriptorRange.BaseShaderRegister = 0;
			descriptorRange.NumDescriptors = 1;
			descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			descriptorRange.OffsetInDescriptorsFromTableStart =
				D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

			D3D12_ROOT_PARAMETER parameters[3]{}; // Material、Transform、Texture
			parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			parameters[0].Descriptor.ShaderRegister = 0;
			parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
			parameters[1].Descriptor.ShaderRegister = 0;
			parameters[2].ParameterType =
				D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			parameters[2].DescriptorTable.pDescriptorRanges = &descriptorRange;
			parameters[2].DescriptorTable.NumDescriptorRanges = 1;

			D3D12_STATIC_SAMPLER_DESC sampler{}; // ミニマップ用Clamp Sampler
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.ShaderRegister = 0;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC description{}; // RootSignature設定
			description.Flags =
				D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
			description.pParameters = parameters;
			description.NumParameters = _countof(parameters);
			description.pStaticSamplers = &sampler;
			description.NumStaticSamplers = 1;

			Microsoft::WRL::ComPtr<ID3DBlob> signature; // 生成済みSignature
			Microsoft::WRL::ComPtr<ID3DBlob> error;     // 失敗時の詳細
			const HRESULT serializeResult = D3D12SerializeRootSignature(
				&description,
				D3D_ROOT_SIGNATURE_VERSION_1,
				&signature,
				&error
			);
			assert(SUCCEEDED(serializeResult));

			const HRESULT createResult = dxCommon_->GetDevice()
				->CreateRootSignature(
					0,
					signature->GetBufferPointer(),
					signature->GetBufferSize(),
					IID_PPV_ARGS(&rootSignature_)
				);
			assert(SUCCEEDED(createResult));
		}

		/// <summary>
		/// 深度なしの画面Overlay用PipelineStateを作成する。
		/// </summary>
		void CreatePipeline() {
			D3D12_INPUT_ELEMENT_DESC elements[3]{}; // Sprite Shader互換Layout
			elements[0] = {
				"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
				D3D12_APPEND_ALIGNED_ELEMENT,
				D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
			};
			elements[1] = {
				"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
				D3D12_APPEND_ALIGNED_ELEMENT,
				D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
			};
			elements[2] = {
				"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
				D3D12_APPEND_ALIGNED_ELEMENT,
				D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
			};
			D3D12_INPUT_LAYOUT_DESC inputLayout{
				elements,
				_countof(elements)
			};

			const auto vertexShader = dxCommon_->CompileShader(
				L"resources/shaders/Sprite.VS.hlsl",
				L"vs_6_0"
			);
			const auto pixelShader = dxCommon_->CompileShader(
				L"resources/shaders/Sprite.PS.hlsl",
				L"ps_6_0"
			);
			assert(vertexShader && pixelShader);

			D3D12_BLEND_DESC blend{}; // 通常αブレンド
			blend.RenderTarget[0].RenderTargetWriteMask =
				D3D12_COLOR_WRITE_ENABLE_ALL;
			blend.RenderTarget[0].BlendEnable = TRUE;
			blend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
			blend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
			blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
			blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
			blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;

			D3D12_RASTERIZER_DESC rasterizer{}; // 両面描画
			rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
			rasterizer.CullMode = D3D12_CULL_MODE_NONE;
			rasterizer.DepthClipEnable = TRUE;

			D3D12_DEPTH_STENCIL_DESC depth{}; // Overlay用なので深度なし
			depth.DepthEnable = FALSE;
			depth.StencilEnable = FALSE;

			D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{}; // PSO設定
			desc.pRootSignature = rootSignature_.Get();
			desc.InputLayout = inputLayout;
			desc.VS = {
				vertexShader->GetBufferPointer(),
				vertexShader->GetBufferSize()
			};
			desc.PS = {
				pixelShader->GetBufferPointer(),
				pixelShader->GetBufferSize()
			};
			desc.BlendState = blend;
			desc.RasterizerState = rasterizer;
			desc.DepthStencilState = depth;
			desc.NumRenderTargets = 1;
			desc.RTVFormats[0] = RenderFormats::kDisplayFormat;
			desc.PrimitiveTopologyType =
				D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			desc.SampleDesc.Count = 1;
			desc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

			const HRESULT createResult = dxCommon_->GetDevice()
				->CreateGraphicsPipelineState(
					&desc,
					IID_PPV_ARGS(&pipelineState_)
				);
			assert(SUCCEEDED(createResult));
		}

		DirectXCommon* dxCommon_ = nullptr; // DirectX共通機能の非所有参照
		Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_; // RootSignature
		Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState_;  // HUD用PSO
		bool initialized_ = false; // Pipeline初期化済みフラグ
	};

	/// <summary>
	/// ミニマップ用クアッドPipelineの共有インスタンスを返す。
	/// </summary>
	MiniMapQuadPipeline& GetMiniMapQuadPipeline() {
		static MiniMapQuadPipeline pipeline; // 全Sceneで共有するPipeline
		return pipeline;
	}
}

void SceneMiniMapSystem::Initialize(
	DirectXCommon* dxCommon,
	SrvManager* srvManager
) {
	dxCommon_ = dxCommon;
	srvManager_ = srvManager;
	initialized_ = dxCommon_ != nullptr && srvManager_ != nullptr;
	if (!initialized_) {
		return;
	}
	GetMiniMapQuadPipeline().Initialize(dxCommon_);
	EnsureQuadResources();
	EnsureSolidTexture();
	EnsureCircleTexture();
}

void SceneMiniMapSystem::DrawOffscreen(
	const SceneDocument* document,
	const DrawSceneCallback& drawScene
) {
	(void)document;
	(void)drawScene;
}

bool SceneMiniMapSystem::HasScreenOverlay(
	const SceneDocument* document
) const {
	if (!initialized_ || !document) {
		return false;
	}
	return ResolveTargetEntity(*document) != nullptr;
}

void SceneMiniMapSystem::DrawScreenOverlay(
	const SceneDocument* document,
	uint32_t canvasWidth,
	uint32_t canvasHeight
) {
	if (
		!initialized_ ||
		!document
	) {
		return;
	}

	const SceneEntity* targetEntity = ResolveTargetEntity(*document);
	if (!targetEntity) {
		return;
	}

	EnsureQuadResources();
	EnsureSolidTexture();
	EnsureCircleTexture();
	if (
		!TextureManager::GetInstance()->HasTexture(solidTextureKey_) ||
		!TextureManager::GetInstance()->HasTexture(circleTextureKey_)
	) {
		return;
	}

	quadDrawIndex_ = 0;

	const float mapSize = (std::min)(
		mapSize_,
		static_cast<float>((std::min)(canvasWidth, canvasHeight)) * 0.32f
	);
	const Vector2 mapPosition{
		static_cast<float>(canvasWidth) - screenMargin_ - mapSize,
		screenMargin_
	};
	const Vector2 mapCenter{
		mapPosition.x + mapSize * 0.5f,
		mapPosition.y + mapSize * 0.5f
	};

	DrawSolidQuad(
		{ mapCenter.x, mapCenter.y },
		{ mapSize + 12.0f, mapSize + 12.0f },
		{ 0.5f, 0.5f },
		0.0f,
		{ 0.02f, 0.025f, 0.03f, 0.64f },
		canvasWidth,
		canvasHeight
	);

	const float border = 2.0f;
	DrawSolidQuad(
		{ mapCenter.x, mapPosition.y },
		{ mapSize + border * 2.0f, border },
		{ 0.5f, 0.5f },
		0.0f,
		{ 0.72f, 0.88f, 1.0f, 0.88f },
		canvasWidth,
		canvasHeight
	);
	DrawSolidQuad(
		{ mapCenter.x, mapPosition.y + mapSize },
		{ mapSize + border * 2.0f, border },
		{ 0.5f, 0.5f },
		0.0f,
		{ 0.72f, 0.88f, 1.0f, 0.88f },
		canvasWidth,
		canvasHeight
	);
	DrawSolidQuad(
		{ mapPosition.x, mapCenter.y },
		{ border, mapSize + border * 2.0f },
		{ 0.5f, 0.5f },
		0.0f,
		{ 0.72f, 0.88f, 1.0f, 0.88f },
		canvasWidth,
		canvasHeight
	);
	DrawSolidQuad(
		{ mapPosition.x + mapSize, mapCenter.y },
		{ border, mapSize + border * 2.0f },
		{ 0.5f, 0.5f },
		0.0f,
		{ 0.72f, 0.88f, 1.0f, 0.88f },
		canvasWidth,
		canvasHeight
	);

	DrawMarkers(
		*document,
		*targetEntity,
		mapPosition,
		mapSize,
		canvasWidth,
		canvasHeight
	);
}

void SceneMiniMapSystem::Finalize() {
	renderTarget_.reset();
	camera_.reset();
	vertexResource_.Reset();
	indexResource_.Reset();
	materialResource_.Reset();
	transformResource_.Reset();
	vertexData_ = nullptr;
	indexData_ = nullptr;
	materialData_ = nullptr;
	transformData_ = nullptr;
	vertexBufferView_ = {};
	indexBufferView_ = {};
	materialStride_ = 0;
	transformStride_ = 0;
	quadDrawIndex_ = 0;
	initialized_ = false;
}

void SceneMiniMapSystem::EnsureRenderResources() {
	if (!camera_) {
		camera_ = std::make_unique<Camera>();
	}
	if (!renderTarget_) {
		renderTarget_ = std::make_unique<SceneRenderTarget>();
		SceneRenderTarget::Desc desc{}; // ミニマップ用RT設定
		desc.width = kMiniMapTextureSize;
		desc.height = kMiniMapTextureSize;
		desc.format = RenderFormats::kSceneHdrFormat;
		desc.createDepth = true;
		desc.clearColor[0] = 0.025f;
		desc.clearColor[1] = 0.032f;
		desc.clearColor[2] = 0.040f;
		desc.clearColor[3] = 1.0f;
		renderTarget_->Initialize(dxCommon_, srvManager_, desc);
	}
}

void SceneMiniMapSystem::EnsureQuadResources() {
	if (vertexResource_) {
		return;
	}

	materialStride_ = AlignTo(
		static_cast<uint32_t>(sizeof(Material)),
		kConstantBufferAlignment
	);
	transformStride_ = AlignTo(
		static_cast<uint32_t>(sizeof(TransformationMatrix)),
		kConstantBufferAlignment
	);

	vertexResource_ = dxCommon_->CreateBufferResource(
		sizeof(VertexData) * 4 * kMaxMiniMapQuadCount
	);
	indexResource_ = dxCommon_->CreateBufferResource(
		sizeof(uint32_t) * 6 * kMaxMiniMapQuadCount
	);
	materialResource_ = dxCommon_->CreateBufferResource(
		materialStride_ * kMaxMiniMapQuadCount
	);
	transformResource_ = dxCommon_->CreateBufferResource(
		transformStride_ * kMaxMiniMapQuadCount
	);

	vertexResource_->Map(0, nullptr, reinterpret_cast<void**>(&vertexData_));
	indexResource_->Map(0, nullptr, reinterpret_cast<void**>(&indexData_));
	materialResource_->Map(0, nullptr, reinterpret_cast<void**>(&materialData_));
	transformResource_->Map(0, nullptr, reinterpret_cast<void**>(&transformData_));

	vertexBufferView_.BufferLocation =
		vertexResource_->GetGPUVirtualAddress();
	vertexBufferView_.SizeInBytes =
		sizeof(VertexData) * 4 * kMaxMiniMapQuadCount;
	vertexBufferView_.StrideInBytes = sizeof(VertexData);
	indexBufferView_.BufferLocation = indexResource_->GetGPUVirtualAddress();
	indexBufferView_.SizeInBytes =
		sizeof(uint32_t) * 6 * kMaxMiniMapQuadCount;
	indexBufferView_.Format = DXGI_FORMAT_R32_UINT;

	for (uint32_t quadIndex = 0; quadIndex < kMaxMiniMapQuadCount; ++quadIndex) {
		const uint32_t indexOffset = quadIndex * 6;  // 対象クアッドのIndex先頭
		indexData_[indexOffset + 0] = 0;
		indexData_[indexOffset + 1] = 1;
		indexData_[indexOffset + 2] = 2;
		indexData_[indexOffset + 3] = 1;
		indexData_[indexOffset + 4] = 3;
		indexData_[indexOffset + 5] = 2;
	}
}

void SceneMiniMapSystem::EnsureSolidTexture() {
	TextureManager* textureManager = TextureManager::GetInstance();
	if (!textureManager || textureManager->HasTexture(solidTextureKey_)) {
		return;
	}

	const uint8_t pixel[4] = { 255, 255, 255, 255 };
	textureManager->UpdateTextureFromPixels(
		solidTextureKey_,
		pixel,
		1,
		1
	);
}

void SceneMiniMapSystem::EnsureCircleTexture() {
	TextureManager* textureManager = TextureManager::GetInstance();
	if (!textureManager || textureManager->HasTexture(circleTextureKey_)) {
		return;
	}

	std::vector<uint8_t> pixels(
		kMiniMapCircleTextureSize * kMiniMapCircleTextureSize * 4,
		0
	); // BGRAの円テクスチャ画素
	const float center =
		(static_cast<float>(kMiniMapCircleTextureSize) - 1.0f) * 0.5f; // 円テクスチャの中心座標
	const float radius = center - 1.0f; // 円の基準半径
	const float edgeWidth = 1.5f; // 円周のアンチエイリアス幅
	for (uint32_t y = 0; y < kMiniMapCircleTextureSize; ++y) {
		for (uint32_t x = 0; x < kMiniMapCircleTextureSize; ++x) {
			const float dx = static_cast<float>(x) - center; // 中心からのX距離
			const float dy = static_cast<float>(y) - center; // 中心からのY距離
			const float distance = std::sqrt(dx * dx + dy * dy); // 中心距離
			const float alpha = std::clamp(
				(radius + edgeWidth - distance) / edgeWidth,
				0.0f,
				1.0f
			); // 円内の不透明度
			const uint32_t index =
				(y * kMiniMapCircleTextureSize + x) * 4; // 画素先頭Index
			pixels[index + 0] = 255;
			pixels[index + 1] = 255;
			pixels[index + 2] = 255;
			pixels[index + 3] = static_cast<uint8_t>(alpha * 255.0f);
		}
	}

	textureManager->UpdateTextureFromPixels(
		circleTextureKey_,
		pixels.data(),
		kMiniMapCircleTextureSize,
		kMiniMapCircleTextureSize
	);
}

const SceneEntity* SceneMiniMapSystem::ResolveTargetEntity(
	const SceneDocument& document
) const {
	for (const SceneEntity& entity : document.GetEntities()) {
		if (
			IsEntityActiveInHierarchy(document, entity) &&
			FindEnabledComponent(entity, "PlayerBehavior")
		) {
			return &entity;
		}
	}
	if (const SceneEntity* player = document.FindEntityByName("Player")) {
		if (IsEntityActiveInHierarchy(document, *player)) {
			return player;
		}
	}
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!entity.folder && IsEntityActiveInHierarchy(document, entity)) {
			return &entity;
		}
	}
	return nullptr;
}

void SceneMiniMapSystem::ConfigureCamera(const Vector3& targetPosition) {
	const Vector3 cameraPosition{
		targetPosition.x,
		targetPosition.y + cameraHeight_,
		targetPosition.z
	};
	const float halfRange = (std::max)(worldRange_ * 0.5f, 1.0f);
	const float fovY = 2.0f * std::atan(halfRange / cameraHeight_);
	camera_->SetLookAt(cameraPosition, targetPosition);
	camera_->SetFovY(std::clamp(fovY, 0.18f, 1.20f));
	camera_->SetAspectRatio(1.0f);
	camera_->SetNearClip(0.1f);
	camera_->SetFarClip(cameraHeight_ + 220.0f);
	camera_->UpdatePreviewMatrices();
}

void SceneMiniMapSystem::DrawTexturedQuad(
	D3D12_GPU_DESCRIPTOR_HANDLE texture,
	const Vector2& position,
	const Vector2& size,
	const Vector2& pivot,
	float rotation,
	const Vector4& color,
	uint32_t canvasWidth,
	uint32_t canvasHeight
) {
	if (!texture.ptr || !vertexData_ || !materialData_ || !transformData_) {
		return;
	}
	if (quadDrawIndex_ >= kMaxMiniMapQuadCount) {
		return;
	}

	const uint32_t quadIndex = quadDrawIndex_++; // 今回使用する描画スロット
	VertexData* vertexData = vertexData_ + quadIndex * 4; // 今回の頂点書き込み先
	Material* materialData = reinterpret_cast<Material*>(
		materialData_ + materialStride_ * quadIndex
	); // 今回のMaterial書き込み先
	TransformationMatrix* transformData =
		reinterpret_cast<TransformationMatrix*>(
			transformData_ + transformStride_ * quadIndex
		); // 今回のTransform書き込み先

	const float left = -pivot.x; // 左端のPivot補正済み座標
	const float right = 1.0f - pivot.x; // 右端のPivot補正済み座標
	const float top = -pivot.y; // 上端のPivot補正済み座標
	const float bottom = 1.0f - pivot.y; // 下端のPivot補正済み座標

	vertexData[0] = {
		{ left, bottom, 0.0f, 1.0f },
		{ 0.0f, 1.0f },
		{ 0.0f, 0.0f, -1.0f }
	};
	vertexData[1] = {
		{ left, top, 0.0f, 1.0f },
		{ 0.0f, 0.0f },
		{ 0.0f, 0.0f, -1.0f }
	};
	vertexData[2] = {
		{ right, bottom, 0.0f, 1.0f },
		{ 1.0f, 1.0f },
		{ 0.0f, 0.0f, -1.0f }
	};
	vertexData[3] = {
		{ right, top, 0.0f, 1.0f },
		{ 1.0f, 0.0f },
		{ 0.0f, 0.0f, -1.0f }
	};

	materialData->color = color;
	materialData->enableLighting = 0;
	materialData->uvTransform = MakeIdentity4x4();

	const Vector3 quadScale{ size.x, size.y, 1.0f }; // 矩形の画面サイズ
	const Vector3 quadRotate{ 0.0f, 0.0f, rotation }; // 矩形の回転角
	const Vector3 quadTranslate{ position.x, position.y, 0.0f }; // 矩形の描画位置
	const Matrix4x4 world = MakeAffineMatrix(
		quadScale,
		quadRotate,
		quadTranslate
	);
	const Matrix4x4 projection = MakeOrthographicMatrix(
		0.0f,
		0.0f,
		static_cast<float>((std::max)(canvasWidth, 1u)),
		static_cast<float>((std::max)(canvasHeight, 1u)),
		0.0f,
		100.0f
	);
	transformData->WVP = Multiply(world, projection);
	transformData->World = MakeIdentity4x4();

	GetMiniMapQuadPipeline().SetCommonRenderState();
	ID3D12GraphicsCommandList* commandList = dxCommon_->GetCommandList();
	D3D12_VERTEX_BUFFER_VIEW vertexBufferView = vertexBufferView_; // 今回のVBV
	vertexBufferView.BufferLocation +=
		sizeof(VertexData) * 4 * quadIndex;
	vertexBufferView.SizeInBytes = sizeof(VertexData) * 4;
	D3D12_INDEX_BUFFER_VIEW indexBufferView = indexBufferView_; // 今回のIBV
	indexBufferView.BufferLocation += sizeof(uint32_t) * 6 * quadIndex;
	indexBufferView.SizeInBytes = sizeof(uint32_t) * 6;
	commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
	commandList->IASetIndexBuffer(&indexBufferView);
	commandList->SetGraphicsRootConstantBufferView(
		0,
		materialResource_->GetGPUVirtualAddress() + materialStride_ * quadIndex
	);
	commandList->SetGraphicsRootConstantBufferView(
		1,
		transformResource_->GetGPUVirtualAddress() + transformStride_ * quadIndex
	);
	commandList->SetGraphicsRootDescriptorTable(2, texture);
	commandList->DrawIndexedInstanced(6, 1, 0, 0, 0);
}

void SceneMiniMapSystem::DrawSolidQuad(
	const Vector2& position,
	const Vector2& size,
	const Vector2& pivot,
	float rotation,
	const Vector4& color,
	uint32_t canvasWidth,
	uint32_t canvasHeight
) {
	TextureManager* textureManager = TextureManager::GetInstance();
	if (!textureManager || !textureManager->HasTexture(solidTextureKey_)) {
		return;
	}
	DrawTexturedQuad(
		textureManager->GetSrvHandleGPU(solidTextureKey_),
		position,
		size,
		pivot,
		rotation,
		color,
		canvasWidth,
		canvasHeight
	);
}

void SceneMiniMapSystem::DrawCircleMarker(
	const Vector2& position,
	float diameter,
	const Vector4& color,
	uint32_t canvasWidth,
	uint32_t canvasHeight
) {
	TextureManager* textureManager = TextureManager::GetInstance();
	if (!textureManager || !textureManager->HasTexture(circleTextureKey_)) {
		return;
	}
	DrawTexturedQuad(
		textureManager->GetSrvHandleGPU(circleTextureKey_),
		position,
		{ diameter, diameter },
		{ 0.5f, 0.5f },
		0.0f,
		color,
		canvasWidth,
		canvasHeight
	);
}

float SceneMiniMapSystem::CalculateMarkerDiameter(
	const Transform& transform,
	float mapSize,
	float minDiameter,
	float maxDiameter
) const {
	const float scaleX = std::abs(transform.scale.x); // EntityのX方向scale
	const float scaleZ = std::abs(transform.scale.z); // EntityのZ方向scale
	const float worldDiameter = (std::max)(
		(std::max)(scaleX, scaleZ),
		0.01f
	); // マップ平面上での代表サイズ
	const float mapWorldRange = (std::max)(worldRange_, 1.0f); // 表示範囲の下限保証
	const float projectedDiameter =
		worldDiameter / mapWorldRange * mapSize; // ミニマップ上の換算直径
	return std::clamp(projectedDiameter, minDiameter, maxDiameter);
}

void SceneMiniMapSystem::DrawMarkers(
	const SceneDocument& document,
	const SceneEntity& targetEntity,
	const Vector2& mapPosition,
	float mapSize,
	uint32_t canvasWidth,
	uint32_t canvasHeight
) {
	const Transform targetTransform =
		ResolveScene3DTransform(document, targetEntity);
	const Vector3 targetPosition = targetTransform.translate;
	const float targetYaw = targetTransform.rotate.y;

	for (const SceneEntity& entity : document.GetEntities()) {
		if (
			entity.id == targetEntity.id ||
			entity.folder ||
			!IsEntityActiveInHierarchy(document, entity)
		) {
			continue;
		}

		Vector4 color{}; // マーカー色
		float diameter = 0.0f; // マーカー直径
		bool useSquareMarker = false; // trueなら四角マーカーとして描画
		const Transform entityTransform =
			ResolveScene3DTransform(document, entity); // 対象EntityのワールドTransform
		if (HasComponent(entity, "EnemySpawner")) {
			color = { 1.0f, 0.82f, 0.25f, 0.95f };
			diameter = CalculateMarkerDiameter(entityTransform, mapSize, 6.0f, 16.0f);
		} else if (HasComponent(entity, "EnemyBehavior")) {
			color = { 1.0f, 0.25f, 0.22f, 0.95f };
			diameter = CalculateMarkerDiameter(entityTransform, mapSize, 6.0f, 14.0f);
		} else if (HasComponent(entity, "FishingHook")) {
			color = { 1.0f, 0.92f, 0.30f, 0.95f };
			diameter = CalculateMarkerDiameter(entityTransform, mapSize, 4.0f, 10.0f);
		} else if (HasComponent(entity, "FishingObstacle")) {
			color = { 0.68f, 0.90f, 1.0f, 0.88f };
			diameter = CalculateMarkerDiameter(entityTransform, mapSize, 8.0f, 22.0f);
			useSquareMarker = true;
		} else if (entity.name.find("PlayerFish") != std::string::npos) {
			color = { 0.25f, 0.80f, 1.0f, 0.92f };
			diameter = CalculateMarkerDiameter(entityTransform, mapSize, 5.0f, 12.0f);
		} else {
			continue;
		}

		const Vector3 worldPosition = entityTransform.translate;
		Vector2 screenPosition{}; // 変換後の画面座標
		if (!ProjectWorldToMap(
			worldPosition,
			targetPosition,
			targetYaw,
			mapPosition,
			mapSize,
			screenPosition
		)) {
			continue;
		}
		if (useSquareMarker) {
			DrawSolidQuad(
				screenPosition,
				{ diameter, diameter },
				{ 0.5f, 0.5f },
				0.0f,
				color,
				canvasWidth,
				canvasHeight
			);
		} else {
			DrawCircleMarker(
				screenPosition,
				diameter,
				color,
				canvasWidth,
				canvasHeight
			);
		}
	}

	Vector2 playerPosition{}; // Playerマーカーの画面座標
	if (ProjectWorldToMap(
		targetPosition,
		targetPosition,
		targetYaw,
		mapPosition,
		mapSize,
		playerPosition
	)) {
		const float playerOuterDiameter =
			CalculateMarkerDiameter(targetTransform, mapSize, 10.0f, 20.0f); // Player外円
		const float playerInnerDiameter =
			(std::max)(playerOuterDiameter - 5.0f, 6.0f); // Player内円
		DrawCircleMarker(
			playerPosition,
			playerOuterDiameter,
			{ 0.02f, 0.04f, 0.04f, 0.90f },
			canvasWidth,
			canvasHeight
		);
		DrawCircleMarker(
			playerPosition,
			playerInnerDiameter,
			{ 0.20f, 1.0f, 0.72f, 1.0f },
			canvasWidth,
			canvasHeight
		);
	}
}

bool SceneMiniMapSystem::ProjectWorldToMap(
	const Vector3& worldPosition,
	const Vector3& targetPosition,
	float targetYaw,
	const Vector2& mapPosition,
	float mapSize,
	Vector2& outScreenPosition
) const {
	const Vector3 relative{
		worldPosition.x - targetPosition.x,
		0.0f,
		worldPosition.z - targetPosition.z
	};
	float mapX = relative.x;
	float mapY = -relative.z;
	if (rotateWithTarget_) {
		const float sinYaw = std::sin(-targetYaw);
		const float cosYaw = std::cos(-targetYaw);
		const float rotatedX = relative.x * cosYaw - relative.z * sinYaw;
		const float rotatedZ = relative.x * sinYaw + relative.z * cosYaw;
		mapX = rotatedX;
		mapY = -rotatedZ;
	}

	const float halfRange = (std::max)(worldRange_ * 0.5f, 1.0f);
	float normalizedX = mapX / halfRange;
	float normalizedY = mapY / halfRange;
	normalizedX = std::clamp(normalizedX, -0.94f, 0.94f);
	normalizedY = std::clamp(normalizedY, -0.94f, 0.94f);

	outScreenPosition = {
		mapPosition.x + mapSize * 0.5f + normalizedX * mapSize * 0.5f,
		mapPosition.y + mapSize * 0.5f + normalizedY * mapSize * 0.5f
	};
	return true;
}
