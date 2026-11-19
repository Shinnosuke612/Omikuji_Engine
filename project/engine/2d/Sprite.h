// 役割: 2DスプライトのTransform、UV、色、描画情報を保持する。
#pragma once
#include "../scene/SceneRuntimePreparation.h"
#include "../math/Vector2.h"
#include "../math/Vector3.h"
#include "../math/Vector4.h"
#include "../math/Transform.h"
#include "../math/Matrix4x4.h"
#include <stdint.h>
#include <d3d12.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <string>
class SpriteCommon;

class Sprite{
private://インナークラス
	struct VertexData{
		Vector4 position;
		Vector2 texcoord;
		Vector3 normal;
	};

	struct Material{
		Vector4 color;
		int32_t enableLighting;
		float padding[3];
		Matrix4x4 uvTransform;
	};

	struct TransformationMatrix{
		Matrix4x4 WVP;
		Matrix4x4 World;
	};
public://公開メンバ関数
	//初期化
	// main専用。公開済み資源を借用し、Readyまで描画しない。
	bool BeginPreparedInitialize(SpriteCommon& common, const std::string& publishedTextureKey,
		std::string& diagnostic);
	RuntimePrepareStep StepPreparedInitialize(SceneRuntimePrepareBudget& remaining, std::string& diagnostic);
	void Initialize(SpriteCommon* spriteCommon,std::string textureFilePath);
	//更新処理
	void Update();
	//描画処理
	// View別snapshotをBindし、UI編集描画による通常Sceneの定数上書きを防ぐ。
	void Draw(bool editorView = false);
	// [0,1] quad→clipの行列を使用する。反転は枠外へ移動せずUVへ適用する。
	void UpdateUI(const Matrix4x4& unitQuadToClip, const Vector4& color);

	//getter
	const Vector2& GetPosition() const{return position;}//座標
	float GetRotation() const{return rotation;}//回転
	const Vector4& GetColor() const{return materialData->color;}//カラー
	const Vector2& GetSize() const{return size;}//サイズ
	const Vector2& GetAnchorPoint() const{return anchorPoint;}//アンカーポイント
	const bool& GetIsFlipX() const{return isFlipX_;}//左右フリップ
	const bool& GetIsFlipY() const{return isFlipY_;}//上下フリップ
	const Vector2& GetTextureLeftTop() const{return textureLeftTop_;}//テクスチャ左上座標
	const Vector2& GetTextureSize() const{return textureSize_;}//テクスチャサイズ

	//setter
	void SetPosition(const Vector2& position){this->position = position;}//座標
	void SetRotation(float rotation){this->rotation = rotation;}//回転
	void SetColor(const Vector4& color){this->materialData->color = color;}//カラー
	void SetSize(const Vector2& size){this->size = size;}//サイズ
	void SetAnchorPoint(const Vector2& anchorPoint){this->anchorPoint = anchorPoint;}//アンカーポイント
	void SetIsFlipX(const bool& isFlipX){isFlipX_ = isFlipX;}//左右フリップ
	void SetIsFlipY(const bool& isFlipY){isFlipY_ = isFlipY;}//上下フリップ
	void SetTextureLeftTop(const Vector2& textureLeftTop){textureLeftTop_ = textureLeftTop;}//テクスチャ左上座標
	void SetTextureSize(const Vector2& textureSize){textureSize_ = textureSize;}//テクスチャサイズ


private://非公開メンバ関数

	struct ViewDrawBuffers {
		Microsoft::WRL::ComPtr<ID3D12Resource> vertices, material, transform;
	};
	// texture所有は共有。Draw時の頂点/定数だけを通常ViewとUI編集Viewで分ける。
	mutable ViewDrawBuffers viewDrawBuffers_[2];
	void MakeVertexData();

	void MakeMaterialData();

	void MakeTransformationMatrixData();

	//テクスチャサイズをイメージに合わせる
	void AdjustTextureSize();

private://メンバ変数

	SpriteCommon* spriteCommon_ = nullptr;
	//バッファリソース
	ID3D12Resource* vertexResource;
	ID3D12Resource* indexResource;
	ID3D12Resource* materialResource;
	ID3D12Resource* transformationMatrixResource;
	//バッファリソース内のデータを指すポインタ
	VertexData* vertexData = nullptr;
	int preparedStage_ = 0;
	Microsoft::WRL::ComPtr<ID3D12Resource> preparedResources_[4];
	uint32_t* indexData = nullptr;
	Material* materialData = nullptr;
	TransformationMatrix* transformationMatrixData = nullptr;
	//バッファリソースの使い道wp補足するバッファビュー
	D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
	D3D12_INDEX_BUFFER_VIEW indexBufferView;
	//テクスチャのファイルパス
	std::string textureFilePath_;

	Transform transform;

	//座標
	Vector2 position = { 0.0f };
	//回転
	float rotation = 0.0f;
	//サイズ
	Vector2 size = { 640.0f,360.0f };
	//アンカーポイント
	Vector2 anchorPoint = { 0.0f,0.0f };
	//左右フリップ
	bool isFlipX_ = false;
	bool isFlipY_ = false;
	//テクスチャ左上座標
	Vector2 textureLeftTop_ = { 0.0f,0.0f };
	//テクスチャ切り出しサイズ
	Vector2 textureSize_ = { 100.0f,100.0f };
};

