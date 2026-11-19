// 役割: CPU画像を行帯に分け、専用COPYキューで一件のテクスチャを転送する。
#pragma once

#include "TextureManager.h"

#include <cstdint>
#include <vector>

class SceneCopyBatch;

// main thread専用。初期化済みでIdleのbatchを借り、転送完了か取消回収まで専有する。
// 各Pumpは入る帯をmip/配列面の順で詰め、最大一バッチを提出する。
// 単一stagingを再使用する前と破棄前に、提出済みバッチを回収する。
// COPY障害時のGPU参照はSceneCopyBatchのteardown契約に従う。
class SceneTextureCopy final {
public:
	enum class Status { Idle, Pending, Copied, Cancelled, Failed };

	SceneTextureCopy() = default;
	SceneTextureCopy(const SceneTextureCopy&) = delete;
	SceneTextureCopy& operator=(const SceneTextureCopy&) = delete;

	// 一回使用。成功時だけimageの所有を移す。予算は行/配置のpaddingを含む。
	// 実staging割当は従来どおりmin(予算, 最大subresourceの行padding込みbytes)。
	bool Begin(ID3D12Device& device, SceneCopyBatch& batch,
		TextureManager::DecodedTexture&& image, uint64_t maxStagingBytes, std::string& error,
		SceneTextureMipPolicy mipPolicy = SceneTextureMipPolicy::CpuComplete);
	Status Pump(std::string& error);
	// 取消後も提出済みの帯があればPumpを続けて回収する。
	void Cancel() noexcept;
	// 全COPY完了後に一回だけCOMMON状態のresourceを渡す。失敗時は両出力を保持する。
	// GpuSrgbBox4096ではmetadataは生成先13段。初期化済みはmip0だけで、公開前にDIRECT生成が必要。
	bool TryTakeCopiedTexture(Microsoft::WRL::ComPtr<ID3D12Resource>& outResource,
		DirectX::TexMetadata& outMetadata) noexcept;

private:
	struct Subresource {
		D3D12_SUBRESOURCE_FOOTPRINT footprint;
		UINT width;
		UINT height;
		UINT rowCount;
		size_t rowBytes;
	};

	void ReleaseCopyInputs() noexcept;
	Status Fail(std::string& error, const char* reason);

	SceneCopyBatch* batch_ = nullptr;
	DirectX::ScratchImage image_;
	DirectX::TexMetadata metadata_{};
	size_t sourceMipLevels_ = 0; // GPU生成ではsource1/target13を分離する。
	std::vector<Subresource> subresources_;
	Microsoft::WRL::ComPtr<ID3D12Resource> texture_;
	Microsoft::WRL::ComPtr<ID3D12Resource> staging_;
	uint64_t stagingBytes_ = 0;
	UINT subresource_ = 0;
	// 非圧縮ではpixel行、BCではblock行。texel座標への変換は記録時だけ行う。
	UINT firstRow_ = 0;
	// 提出成功時に終点を保留し、Fence完了時だけ確定cursorへ移す。
	UINT submittedSubresource_ = 0;
	UINT submittedFirstRow_ = 0;
	UINT texelsPerRow_ = 1;
	Status status_ = Status::Idle;
	bool copyInFlight_ = false;
	bool cancelRequested_ = false;
};
