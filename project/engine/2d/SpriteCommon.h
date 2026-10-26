// 役割: スプライト描画で共有するPipelineStateとRootSignatureを管理する。
#pragma once
#include <d3dx12.h>
#include <wrl.h>
class DirectXCommon;

class SpriteCommon{
private:
	static SpriteCommon* instance_;

	SpriteCommon() = default;
	~SpriteCommon() = default;
	SpriteCommon(const SpriteCommon&) = delete;
	SpriteCommon& operator=(const SpriteCommon&) = delete;
public://公開メンバ関数
	enum class OutputTarget { SceneHdr, Display };

	// インスタンス取得 / 削除
	static SpriteCommon* GetInstance();
	static void DeleteInstance();

	//初期化
	void Initialize(DirectXCommon* dxCommon);
	//共通描画設定
	// 既存呼出しはHDRを維持。新UIはDisplay/HDRとも深度なしのPSOを選ぶ。
	void SetCommonRenderState(OutputTarget target = OutputTarget::SceneHdr, bool ui = false);

public://ゲッター
	DirectXCommon* GetDxCommon() const{ return dxCommon_; }

private://非公開メンバ関数

	//ルートシグネチャの作成
	void MakeRootSignature();
	//グラフィックパイプラインの生成
	void GenerateGraphicsPipeline();

	// どこか共通ヘルパに
	inline D3D12_STATIC_SAMPLER_DESC MakeStaticSamplerS0();

private://メンバ変数

	//処理用にDirectXCommonを保持する
	DirectXCommon* dxCommon_;
	//ルートシグネチャ
	Microsoft::WRL::ComPtr < ID3D12RootSignature> rootSignature_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipelineState_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> uiHdrPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> displayPipelineState_;
	bool isInitialized_ = false;
private://メンバクラス
	enum class BlendMode{
		//!< ブレンドなし
		kBlendModeNone,
		//!< 通常アルファブレンド
		kBlendModeNormal,
		//!< 加算
		kBlendModeAdd,
		//!< 減算
		kBlendModeSubtract,
		//!< 乗算
		kBlendModeMultiply,
		//!< スクリーン
		kBlendModeScreen,
		//利用してはいけない
		kCountOfBlendMode,
	};
};

