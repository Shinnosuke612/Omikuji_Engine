// 役割: フルスクリーン画像コピーとポストエフェクトの共通描画を管理する。
#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl.h>

#include "../math/Matrix4x4.h"

class DirectXCommon;

class FullscreenCopy {
public:
	enum class Effect {
		kCopy,
		kGrayscale,
		kVignette,
		kBoxBlur,
		kGaussianBlur,
		kRadialBlur,
		kNoise,
		kDissolve,
		kOutline,
		kDepthOfField,
		kUnderwater,
		kWaterRefraction,
		kWaterLightShafts,
		kPixelation,
		kChromaticAberration,
		kMotionBlur,
		kIrisTransition
	};

	enum class OutputFormat {
		kDisplay,
		kSceneHdr
	};

	struct Parameters {
		float vignetteScale = 16.0f;
		float vignettePower = 0.8f;
		float vignetteIntensity = 1.0f;
		float blurStrength = 1.0f;
		uint32_t blurRadius = 1;
		float gaussianSigma = 1.0f;
		float padding[2]{};
		float outlineLuminanceWeight = 1.0f;
		float outlineDepthWeight = 1.0f;
		float outlineThreshold = 0.1f;
		float outlineSoftness = 0.05f;
		float outlineThickness = 1.0f;
		float cameraNear = 0.1f;
		float cameraFar = 1000.0f;
		uint32_t outlineFlags = 0;
		float outlineColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
		float radialBlurCenter[2]{ 0.5f, 0.5f };
		float radialBlurWidth = 0.01f;
		uint32_t radialBlurSamples = 10;
		float dissolveThreshold = 0.0f;
		float dissolveEdgeWidth = 0.03f;
		float dissolvePadding[2]{};
		float dissolveEdgeColor[4]{ 1.0f, 0.4f, 0.3f, 1.0f };
		float noiseTime = 0.0f;
		float noiseAmount = 0.25f;
		float noiseScale = 1.0f;
		float noiseSeed = 0.0f;
		float dofFocusDistance = 10.0f;
		float dofFocusRange = 2.0f;
		float dofNearStrength = 0.0f;
		float dofFarStrength = 1.0f;
		float dofMaxRadius = 4.0f;
		float dofPadding[3]{};
		float underwaterTintColor[4]{ 0.02f, 0.45f, 0.68f, 1.0f };
		float underwaterParams[4]{ 0.0f, 0.035f, 0.012f, 0.0f };
		float cameraUpTime[4]{ 0.0f, 1.0f, 0.0f, 0.0f };
		float cameraPositionFovY[4]{ 0.0f, 0.0f, 0.0f, 0.45f };
		float cameraRightAspect[4]{ 1.0f, 0.0f, 0.0f, 1.0f };
		float cameraForwardActive[4]{ 0.0f, 0.0f, 1.0f, 0.0f };
		float waterVolumeCenterActive[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
		float waterVolumeHalfSizeEdge[4]{ 0.0f, 0.0f, 0.0f, 0.08f };
		float waterRefractionTintColor[4]{ 0.02f, 0.55f, 0.82f, 1.0f };
		float waterRefractionParams[4]{ 0.018f, 0.12f, 0.0f, 0.0f };
		float waterLightColorIntensity[4]{ 0.55f, 0.90f, 1.15f, 0.55f };
		float waterLightDirectionDensity[4]{ -0.25f, -1.0f, 0.18f, 0.045f };
		float waterLightParams[4]{ 0.35f, 0.08f, 1.0f, 16.0f };
		float waterLightNoiseParams[4]{ 1.0f, 1.0f, 1.0f, 0.0f };
		float chromaticCenterIntensity[4]{ 0.5f, 0.5f, 0.003f, 0.0f };
		float chromaticParams[4]{ 1.5f, 0.0f, 0.0f, 0.0f };
		float pixelationParams[4]{ 4.0f, 0.0f, 0.0f, 0.0f };
		Matrix4x4 motionBlurInverseCurrentViewProjection{};
		Matrix4x4 motionBlurPreviousViewProjection{};
		float motionBlurParams[4]{ 0.5f, 8.0f, 24.0f, 0.0f };
		float motionBlurTextureSizeHistoryValid[4]{};
	};

	void Initialize(DirectXCommon* dxCommon);
	void BeginFrame();
	void SetParameters(const Parameters& parameters);
	void Draw(
		D3D12_GPU_DESCRIPTOR_HANDLE textureHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE depthTextureHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE maskTextureHandle,
		Effect effect = Effect::kCopy,
		OutputFormat outputFormat = OutputFormat::kDisplay
	);
	void Draw(
		D3D12_GPU_DESCRIPTOR_HANDLE textureHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE depthTextureHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE maskTextureHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE historyTextureHandle,
		Effect effect,
		OutputFormat outputFormat = OutputFormat::kDisplay
	);

private:
	void CreateRootSignature();
	void CreatePipelineState();

	DirectXCommon* dxCommon_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> copyPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> copySceneHdrPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> grayscalePipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> vignettePipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> boxBlurPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> gaussianBlurPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> radialBlurPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> noisePipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> dissolvePipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> outlinePipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> depthOfFieldPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> underwaterPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> waterRefractionPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> waterLightShaftsPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pixelationPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> chromaticAberrationPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> motionBlurPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> irisTransitionPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState>
		waterRefractionSceneHdrPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState>
		waterLightShaftsSceneHdrPipelineState_;
	static constexpr uint32_t kMaxDrawsPerFrame = 24;
	Microsoft::WRL::ComPtr<ID3D12Resource>
		parameterResources_[kMaxDrawsPerFrame];
	Parameters* parameterData_[kMaxDrawsPerFrame]{};
	Parameters pendingParameters_{};
	uint32_t drawIndex_ = 0;
};
