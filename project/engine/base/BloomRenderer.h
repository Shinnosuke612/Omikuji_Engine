// 役割: 輝度抽出、ぼかし、合成によるBloomポストエフェクトを管理する。
#pragma once

#include <cstdint>

#include <d3d12.h>
#include <wrl.h>

#include "SceneRenderTarget.h"

class DirectXCommon;
class SrvManager;

class BloomRenderer {
public:
	enum class ToneMapMode {
		kAces = 0,
		kReinhard = 1,
	};

	struct Parameters {
		int32_t enabled = 1;
		float threshold = 1.0f;
		float softKnee = 0.5f;
		float intensity = 0.7f;

		float exposure = 1.0f;
		int32_t blurIterations = 4;
		int32_t downsampleScale = 2;
		float blurRadius = 1.0f;

		int32_t toneMapMode = static_cast<int32_t>(ToneMapMode::kAces);
		int32_t horizontal = 0;
		float texelSize[2]{};
		float padding[2]{};
	};

	bool Initialize(DirectXCommon* dxCommon, SrvManager* srvManager);
	void Resize(uint32_t width, uint32_t height, uint32_t downsampleScale);
	void BeginFrame();
	void SetParameters(const Parameters& parameters);
	void Apply(
		D3D12_GPU_DESCRIPTOR_HANDLE sceneTexture,
		SceneRenderTarget* outputTarget
	);

private:
	enum class Pass {
		kExtract,
		kBlur,
		kComposite,
	};

	void CreateRootSignature();
	void CreatePipelineStates();
	void DrawFullscreen(
		D3D12_GPU_DESCRIPTOR_HANDLE textureHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE bloomHandle,
		Pass pass
	);

	DirectXCommon* dxCommon_ = nullptr;
	SrvManager* srvManager_ = nullptr;

	SceneRenderTarget brightTarget_;
	SceneRenderTarget blurTargets_[2];

	Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> extractPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> blurPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> compositePipelineState_;

	static constexpr uint32_t kMaxDrawsPerFrame = 32;
	Microsoft::WRL::ComPtr<ID3D12Resource>
		parameterResources_[kMaxDrawsPerFrame];
	Parameters* parameterData_[kMaxDrawsPerFrame]{};
	Parameters pendingParameters_{};
	uint32_t drawIndex_ = 0;
	uint32_t width_ = 1;
	uint32_t height_ = 1;
	uint32_t downsampleScale_ = 2;
	bool initialized_ = false;
};
