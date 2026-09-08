// 役割: 最小構成のHUDミニマップ描画を管理する。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <d3d12.h>
#include <wrl.h>

#include "../../../engine/3d/Camera.h"
#include "../../../engine/base/SceneRenderTarget.h"
#include "../../../engine/math/Matrix4x4.h"
#include "../../../engine/math/Transform.h"
#include "../../../engine/math/Vector2.h"
#include "../../../engine/math/Vector3.h"
#include "../../../engine/math/Vector4.h"

class DirectXCommon;
class SceneDocument;
class SrvManager;
struct SceneEntity;

// MiniMap Component化前の試作用。RuntimeSceneから呼び出される描画だけを持つ。
class SceneMiniMapSystem {
public:
	using DrawSceneCallback = std::function<void(Camera*, uint64_t)>;

	/// <summary>
	/// DirectX依存を受け取り、ミニマップ描画に必要な固定リソースを準備する。
	/// </summary>
	void Initialize(DirectXCommon* dxCommon, SrvManager* srvManager);

	/// <summary>
	/// 将来の背景描画拡張用に、ミニマップのオフスクリーン描画入口を保持する。
	/// </summary>
	void DrawOffscreen(
		const SceneDocument* document,
		const DrawSceneCallback& drawScene
	);

	/// <summary>
	/// 現在のSceneでミニマップを画面Overlayとして表示できるかを返す。
	/// </summary>
	bool HasScreenOverlay(const SceneDocument* document) const;

	/// <summary>
	/// 半透明背景と簡易マーカーを画面右上へ描画する。
	/// </summary>
	void DrawScreenOverlay(
		const SceneDocument* document,
		uint32_t canvasWidth,
		uint32_t canvasHeight
	);

	/// <summary>
	/// GPUリソースと描画状態を破棄する。
	/// </summary>
	void Finalize();

private:
	struct VertexData {
		Vector4 position; // クアッド頂点のローカル座標
		Vector2 texcoord; // テクスチャ参照座標
		Vector3 normal;   // 既存Sprite Shader互換の法線
	};

	struct Material {
		Vector4 color;        // テクスチャへ乗算する色
		int32_t enableLighting = 0; // Sprite Shader互換の照明フラグ
		float padding[3]{};   // 定数バッファのアラインメント調整
		Matrix4x4 uvTransform; // UV変換行列
	};

	struct TransformationMatrix {
		Matrix4x4 WVP;   // 画面座標へ変換する行列
		Matrix4x4 World; // Sprite Shader互換のWorld行列
	};

	/// <summary>
	/// ミニマップ用RenderTargetとカメラを必要に応じて作成する。
	/// </summary>
	void EnsureRenderResources();

	/// <summary>
	/// HUD用クアッドの頂点、Index、定数バッファを必要に応じて作成する。
	/// </summary>
	void EnsureQuadResources();

	/// <summary>
	/// 枠線とマーカー描画に使う1px白テクスチャを登録する。
	/// </summary>
	void EnsureSolidTexture();

	/// <summary>
	/// ミニマップマーカーに使う白円テクスチャを登録する。
	/// </summary>
	void EnsureCircleTexture();

	/// <summary>
	/// PlayerBehaviorを優先してミニマップの追従対象Entityを探す。
	/// </summary>
	const SceneEntity* ResolveTargetEntity(const SceneDocument& document) const;

	/// <summary>
	/// 追従対象のワールド位置からミニマップ用カメラを設定する。
	/// </summary>
	void ConfigureCamera(const Vector3& targetPosition);

	/// <summary>
	/// 指定Texture SRVを使って画面上へ矩形を描画する。
	/// </summary>
	void DrawTexturedQuad(
		D3D12_GPU_DESCRIPTOR_HANDLE texture,
		const Vector2& position,
		const Vector2& size,
		const Vector2& pivot,
		float rotation,
		const Vector4& color,
		uint32_t canvasWidth,
		uint32_t canvasHeight
	);

	/// <summary>
	/// 1px白テクスチャを色付き矩形として描画する。
	/// </summary>
	void DrawSolidQuad(
		const Vector2& position,
		const Vector2& size,
		const Vector2& pivot,
		float rotation,
		const Vector4& color,
		uint32_t canvasWidth,
		uint32_t canvasHeight
	);

	/// <summary>
	/// 白円テクスチャを使って色付き円マーカーを描画する。
	/// </summary>
	void DrawCircleMarker(
		const Vector2& position,
		float diameter,
		const Vector4& color,
		uint32_t canvasWidth,
		uint32_t canvasHeight
	);

	/// <summary>
	/// Entityのワールドscaleからミニマップ上の円マーカー直径を求める。
	/// </summary>
	float CalculateMarkerDiameter(
		const Transform& transform,
		float mapSize,
		float minDiameter,
		float maxDiameter
	) const;

	/// <summary>
	/// Player、釣り針、岩などの簡易マーカーをミニマップ上へ重ねる。
	/// </summary>
	void DrawMarkers(
		const SceneDocument& document,
		const SceneEntity& targetEntity,
		const Vector2& mapPosition,
		float mapSize,
		uint32_t canvasWidth,
		uint32_t canvasHeight
	);

	/// <summary>
	/// ワールド座標をミニマップ内の画面座標へ変換する。
	/// </summary>
	bool ProjectWorldToMap(
		const Vector3& worldPosition,
		const Vector3& targetPosition,
		float targetYaw,
		const Vector2& mapPosition,
		float mapSize,
		Vector2& outScreenPosition
	) const;

	DirectXCommon* dxCommon_ = nullptr; // DirectX共通機能の非所有参照
	SrvManager* srvManager_ = nullptr;  // SRV管理の非所有参照
	std::unique_ptr<Camera> camera_;    // ミニマップ専用の上空カメラ
	std::unique_ptr<SceneRenderTarget> renderTarget_; // ミニマップ用RenderTarget
	Microsoft::WRL::ComPtr<ID3D12Resource> vertexResource_; // クアッド頂点バッファ
	Microsoft::WRL::ComPtr<ID3D12Resource> indexResource_;  // クアッドIndexバッファ
	Microsoft::WRL::ComPtr<ID3D12Resource> materialResource_; // Material定数バッファ
	Microsoft::WRL::ComPtr<ID3D12Resource> transformResource_; // Transform定数バッファ
	VertexData* vertexData_ = nullptr; // Map済み頂点データ
	uint32_t* indexData_ = nullptr;    // Map済みIndexデータ
	uint8_t* materialData_ = nullptr; // Map済みMaterialデータの先頭
	uint8_t* transformData_ = nullptr; // Map済みTransformデータの先頭
	D3D12_VERTEX_BUFFER_VIEW vertexBufferView_{}; // クアッド用VBV
	D3D12_INDEX_BUFFER_VIEW indexBufferView_{};   // クアッド用IBV
	uint32_t materialStride_ = 0; // Material定数バッファの1描画分Stride
	uint32_t transformStride_ = 0; // Transform定数バッファの1描画分Stride
	uint32_t quadDrawIndex_ = 0; // フレーム内のクアッド描画Index
	std::string solidTextureKey_ = "__minimap_solid_white"; // 色付き矩形用白テクスチャKey
	std::string circleTextureKey_ = "__minimap_marker_circle"; // マーカー用白円テクスチャKey
	Vector3 lastTargetPosition_{}; // 最後に解決した追従対象位置
	float worldRange_ = 48.0f;     // ミニマップに表示するワールド範囲
	float cameraHeight_ = 72.0f;   // 上空カメラの高さ
	float mapSize_ = 192.0f;       // 画面上のミニマップ表示サイズ
	float screenMargin_ = 24.0f;   // 画面端からの余白
	bool rotateWithTarget_ = false; // trueで対象の向きに合わせてマップを回転
	bool initialized_ = false;      // Initialize完了フラグ
};
