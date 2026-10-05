// 役割: GPUパーティクルで共有する粒子状態とGPUリソース定義を提供する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl.h>

#include "../math/Matrix4x4.h"
#include "../math/Vector3.h"
#include "../math/Vector4.h"
#include "ParticleCommon.h"

class Camera;
class DirectXCommon;
struct ParticleEffectDesc;
class SrvManager;

class GpuParticle {
public:
	static constexpr uint32_t kMaxParticles = 20480;

	struct ParticleData {
		Vector3 translate;
		float lifeTime;
		Vector3 scale;
		float currentTime;
		Vector3 rotate;
		float rotationSpeed;
		Vector3 velocity;
		float initialAlpha;
		Vector4 color;
		Vector3 acceleration;
		float padding0;
		Vector3 startScale;
		float padding1;
		Vector3 endScale;
		float padding2;
		Vector4 startColor;
		Vector4 endColor;
		Vector3 swayAxis;
		float swayPhase;
		Vector3 vortexCenter;
		float vortexAngle;
		float vortexRadius;
		float vortexHeightOffset;
		float vortexAngularSpeed;
		float vortexInwardSpeed;
		float vortexVerticalSpeed;
		uint32_t vortexAxis;
		float vortexPadding[2];
	};

	struct Material {
		Vector4 color;
		int32_t enableLighting;
		float alphaCutoff;
		int32_t flipU;
		int32_t flipV;
		Matrix4x4 uvTransform;
		float emissiveIntensity;
		float padding[3];
	};

	struct DirectionalLight {
		Vector4 color;
		Vector3 direction;
		float intensity;
	};

	struct EmitterSphere {
		Vector3 translate;
		float radius;
		Vector3 spawnSize;
		uint32_t shape;
		uint32_t count;
		float frequency;
		float frequencyTime;
		uint32_t emit;
	};

	struct PerView {
		Matrix4x4 viewProjection;
		Matrix4x4 billboardMatrix;
		Vector4 renderFlags;
	};

	struct PerFrame {
		float time;
		float deltaTime;
		float padding[2];
	};

	struct BehaviorSettings {
		float lifeTimeMin = 0.6f;
		float lifeTimeMax = 1.2f;
		float scaleMin = 0.08f;
		float scaleMax = 0.22f;
		Vector3 startScaleMin = { 0.08f, 0.08f, 1.0f };
		float paddingScale0 = 0.0f;
		Vector3 startScaleMax = { 0.22f, 0.22f, 1.0f };
		float paddingScale1 = 0.0f;
		bool enableScaleOverLife = false;
		Vector3 endScaleMin = { 0.08f, 0.08f, 1.0f };
		float paddingScale2 = 0.0f;
		Vector3 endScaleMax = { 0.22f, 0.22f, 1.0f };
		float paddingScale3 = 0.0f;
		float velocityMin = 0.4f;
		float velocityMax = 1.0f;
		Vector3 velocityBase = { 0.0f, 0.0f, 0.0f };
		float padding0 = 0.0f;
		Vector3 velocityRandomRange = { 1.0f, 1.0f, 1.0f };
		float padding1 = 0.0f;
		Vector3 accelerationBase = { 0.0f, 0.0f, 0.0f };
		float padding2 = 0.0f;
		Vector3 accelerationRandomRange = { 0.0f, 0.0f, 0.0f };
		float padding3 = 0.0f;
		float rotationSpeedMin = -4.0f;
		float rotationSpeedMax = 4.0f;
		bool alignToVelocity = false;
		uint32_t alignAxis = 1;
		bool enableLifeFade = true;
		float fadeOutStartRatio = 0.7f;
		uint32_t colorMode = 0;
		Vector4 colorMin = { 0.8f, 0.35f, 0.25f, 0.65f };
		Vector4 colorMax = { 1.0f, 0.75f, 0.45f, 0.95f };
		Vector4 endColorMin = { 0.8f, 0.35f, 0.25f, 0.0f };
		Vector4 endColorMax = { 1.0f, 0.75f, 0.45f, 0.0f };
		float swayAmplitude = 0.0f;
		float swayFrequency = 0.0f;
		bool pointFieldEnabled = false;
		float pointFieldRadius = 0.0f;
		Vector3 pointFieldCenter = { 0.0f, 0.0f, 0.0f };
		float pointFieldAttraction = 0.0f;
		float pointFieldRepulsion = 0.0f;
		float pointFieldOrbit = 0.0f;
		float pointFieldFalloff = 1.0f;
		float pointFieldDamping = 0.0f;
		Vector3 pointFieldOrbitAxis = { 0.0f, 1.0f, 0.0f };
		uint32_t movementMode = 0;
		Vector3 vortexCenter = { 0.0f, 0.0f, 0.0f };
		uint32_t vortexAxis = 1;
		float vortexAngularSpeedMin = 4.0f;
		float vortexAngularSpeedMax = 8.0f;
		float vortexInwardSpeedMin = 0.8f;
		float vortexInwardSpeedMax = 1.8f;
		float vortexVerticalSpeedMin = -0.1f;
		float vortexVerticalSpeedMax = 0.1f;
	};

	struct Config {
		std::string filePath = "resources/particles/gpu_particle.json";
		std::string textureFilePath = "resources/circle.png";
		ParticleCommon::BlendMode blendMode =
			ParticleCommon::BlendMode::kBlendModeAdd;
		bool autoEmit = true;
		bool useBillboard = true;
		bool forceVisible = false;
		EmitterSphere emitter{
			{ -5.0f, 5.0f, 0.0f },
			0.35f,
			{ 1.0f, 1.0f, 1.0f },
			0,
			10,
			0.5f,
			0.0f,
			0
		};
		BehaviorSettings behavior;
	};

	struct BehaviorForGPU {
		Vector4 lifeScaleVelocityMinRotationMin;
		Vector4 lifeScaleVelocityMaxRotationMax;
		Vector4 startScaleMin;
		Vector4 startScaleMax;
		Vector4 endScaleMin;
		Vector4 endScaleMax;
		Vector4 velocityBase;
		Vector4 velocityRandomRange;
		Vector4 accelerationBase;
		Vector4 accelerationRandomRange;
		Vector4 flags;
		Vector4 rotationFlags;
		Vector4 colorMin;
		Vector4 colorMax;
		Vector4 endColorMin;
		Vector4 endColorMax;
		Vector4 sway;
		Vector4 pointFieldFlags;
		Vector4 pointFieldCenter;
		Vector4 pointFieldStrengths;
		Vector4 pointFieldOrbitAxis;
		Vector4 motionFlags;
		Vector4 vortexCenter;
		Vector4 vortexAngularInwardSpeed;
		Vector4 vortexVerticalSpeed;
	};

	struct RuntimeInfo {
		bool initialized = false;
		bool autoEmit = false;
		uint32_t maxParticles = kMaxParticles;
		uint32_t emitCount = 0;
		uint32_t emitFlags = 0;
		float frequency = 0.0f;
		float frequencyTime = 0.0f;
	};

	bool Initialize(
		ParticleCommon* particleCommon,
		SrvManager* srvManager,
		const std::string& textureFilePath = "resources/circle.png"
	);
	void Reset();
	void ClearParticles();
	void Update();
	void Draw(Camera* camera);
	void DrawImGui(const char* windowTitle = "GPU Particle");
	void ApplyEffectDesc(const ParticleEffectDesc& effect);
	void RequestResetBuffer();
	void EmitOnce();
	RuntimeInfo GetRuntimeInfo() const;
	bool LoadConfig(const std::string& filePath);
	bool SaveConfig(const std::string& filePath) const;
	const Config& GetConfig() const { return config_; }
	bool IsInitialized() const;

private:
	bool CreateParticleResource();
	void CreateConstantBuffers();
	void CreateRootSignatures();
	void CreatePipelineStates();
	void InitializeParticlesOnGPU();
	void EmitParticlesOnGPU();
	void UpdateParticlesOnGPU();
	void ApplyConfigToGpu();
	bool ApplyTexture(const std::string& textureFilePath);
	void CopyStringsToBuffers();
	void RefreshTextureFiles();
	void TransitionParticleResource(D3D12_RESOURCE_STATES stateAfter);
	void TransitionFreeListIndexResource(D3D12_RESOURCE_STATES stateAfter);
	void TransitionFreeListResource(D3D12_RESOURCE_STATES stateAfter);

private:
	ParticleCommon* particleCommon_ = nullptr;
	SrvManager* srvManager_ = nullptr;
	DirectXCommon* dxCommon_ = nullptr;

	std::string textureFilePath_;
	uint32_t textureSrvIndex_ = UINT32_MAX;
	uint32_t particleSrvIndex_ = UINT32_MAX;
	uint32_t particleUavIndex_ = UINT32_MAX;
	uint32_t freeListIndexUavIndex_ = UINT32_MAX;
	uint32_t freeListUavIndex_ = UINT32_MAX;

	Microsoft::WRL::ComPtr<ID3D12Resource> particleResource_;
	D3D12_RESOURCE_STATES particleResourceState_ = D3D12_RESOURCE_STATE_COMMON;
	Microsoft::WRL::ComPtr<ID3D12Resource> freeListIndexResource_;
	D3D12_RESOURCE_STATES freeListIndexResourceState_ =
		D3D12_RESOURCE_STATE_COMMON;
	Microsoft::WRL::ComPtr<ID3D12Resource> freeListResource_;
	D3D12_RESOURCE_STATES freeListResourceState_ = D3D12_RESOURCE_STATE_COMMON;

	Microsoft::WRL::ComPtr<ID3D12Resource> materialResource_;
	Material* materialData_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> directionalLightResource_;
	DirectionalLight* directionalLightData_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> perViewResource_;
	PerView* perViewData_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> emitterResource_;
	EmitterSphere* emitterData_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> perFrameResource_;
	PerFrame* perFrameData_ = nullptr;
	Microsoft::WRL::ComPtr<ID3D12Resource> behaviorResource_;
	BehaviorForGPU* behaviorData_ = nullptr;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> graphicsRootSignature_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> graphicsPipelineStates_
		[static_cast<uint32_t>(ParticleCommon::BlendMode::kCountOfBlendMode)];
	Microsoft::WRL::ComPtr<ID3D12RootSignature> initializeRootSignature_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> initializePipelineState_;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> emitRootSignature_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> emitPipelineState_;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> updatePipelineState_;

	bool needsInitialize_ = true;
	float elapsedTime_ = 0.0f;
	float deltaTime_ = 1.0f / 60.0f;
	Config config_;
	char configPathBuffer_[256]{};
	char texturePathBuffer_[256]{};
	std::vector<std::string> textureFilePaths_;
};
