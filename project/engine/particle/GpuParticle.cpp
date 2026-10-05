// 役割: GPUパーティクル用のBuffer、PipelineState、Compute Shader連携を実装する。
#include "GpuParticle.h"
#include "../base/RenderFormats.h"

#include "ParticleEffectResource.h"
#include "ParticleCommon.h"
#include "../2d/TextureManager.h"
#include "../utility/ResourceTextureCatalog.h"
#include "../3d/Camera.h"
#include "../3d/SrvManager.h"
#include "../base/DirectXCommon.h"
#include "../externals/nlohmann/json.hpp"
#include "../utility/EditableResourcePath.h"
#include "../utility/Logger.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

#if defined(_DEBUG) || defined(DEVELOPMENT)
#include <imgui.h>
#endif

using json = nlohmann::json;

namespace {

constexpr float kMinEmitterFrequency = 1.0f / 60.0f;
constexpr uint32_t kEmitFlagEmitParticles = 1u << 0;
constexpr uint32_t kEmitFlagSeedVisibleParticle = 1u << 1;
constexpr uint32_t kGpuParticleThreadCount = 1024;

uint32_t GpuParticleDispatchGroupCount() {
	return (GpuParticle::kMaxParticles + kGpuParticleThreadCount - 1u) /
		kGpuParticleThreadCount;
}

float NormalizeEmitterFrequency(float frequency) {
	return frequency > 0.0f ? frequency : kMinEmitterFrequency;
}

float MaxComponent(const Vector3& value) {
	return (std::max)((std::max)(value.x, value.y), value.z);
}

float LengthSquared(const Vector3& value) {
	return value.x * value.x + value.y * value.y + value.z * value.z;
}

Vector3 NormalizeVectorOrZero(const Vector3& value) {
	const float lengthSquared = LengthSquared(value);
	if (lengthSquared <= 0.000001f) {
		return { 0.0f, 0.0f, 0.0f };
	}

	const float invLength = 1.0f / std::sqrt(lengthSquared);
	return {
		value.x * invLength,
		value.y * invLength,
		value.z * invLength
	};
}

float SanitizeScale(float value, float fallback) {
	if (std::isfinite(value) && value > 0.0f) {
		return value;
	}
	return fallback;
}

std::string FormatHRESULT(HRESULT hr) {
	std::ostringstream stream;
	stream << "0x" << std::uppercase << std::hex
		   << static_cast<unsigned long>(hr);
	return stream.str();
}

bool LogFailedHRESULT(const char* label, HRESULT hr) {
	if (SUCCEEDED(hr)) {
		return false;
	}
	Logger::Log(std::string(label) + " failed. hr=" + FormatHRESULT(hr) + "\n");
	return true;
}

const char* ToString(ParticleCommon::BlendMode mode) {
	switch (mode) {
	case ParticleCommon::BlendMode::kBlendModeNone: return "None";
	case ParticleCommon::BlendMode::kBlendModeNormal: return "Normal";
	case ParticleCommon::BlendMode::kBlendModeAdd: return "Add";
	case ParticleCommon::BlendMode::kBlendModeSubtract: return "Subtract";
	case ParticleCommon::BlendMode::kBlendModeMultiply: return "Multiply";
	case ParticleCommon::BlendMode::kBlendModeScreen: return "Screen";
	default: return "Add";
	}
}

ParticleCommon::BlendMode ToBlendMode(const std::string& text) {
	if (text == "None") return ParticleCommon::BlendMode::kBlendModeNone;
	if (text == "Normal") return ParticleCommon::BlendMode::kBlendModeNormal;
	if (text == "Add") return ParticleCommon::BlendMode::kBlendModeAdd;
	if (text == "Subtract") return ParticleCommon::BlendMode::kBlendModeSubtract;
	if (text == "Multiply") return ParticleCommon::BlendMode::kBlendModeMultiply;
	if (text == "Screen") return ParticleCommon::BlendMode::kBlendModeScreen;
	return ParticleCommon::BlendMode::kBlendModeAdd;
}

void ApplyBlendMode(
	D3D12_BLEND_DESC& blendDesc,
	ParticleCommon::BlendMode blendMode
) {
	blendDesc = {};
	blendDesc.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;

	if (blendMode == ParticleCommon::BlendMode::kBlendModeNone) {
		blendDesc.RenderTarget[0].BlendEnable = FALSE;
		return;
	}

	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	if (blendMode == ParticleCommon::BlendMode::kBlendModeNormal) {
		blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	}
	else if (blendMode == ParticleCommon::BlendMode::kBlendModeAdd) {
		blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
	}
	else if (blendMode == ParticleCommon::BlendMode::kBlendModeSubtract) {
		blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_REV_SUBTRACT;
		blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
	}
	else if (blendMode == ParticleCommon::BlendMode::kBlendModeMultiply) {
		blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_DEST_COLOR;
		blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	}
	else if (blendMode == ParticleCommon::BlendMode::kBlendModeScreen) {
		blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_INV_DEST_COLOR;
		blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
	}

	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
}

Microsoft::WRL::ComPtr<ID3D12Resource> CreateUavBufferResource(
	ID3D12Device* device,
	size_t sizeInBytes
) {
	assert(device);

	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resourceDesc.Width = sizeInBytes;
	resourceDesc.Height = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.MipLevels = 1;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	Microsoft::WRL::ComPtr<ID3D12Resource> resource;
	const HRESULT hr = device->CreateCommittedResource(
		&heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(&resource)
	);
	assert(SUCCEEDED(hr));
	return resource;
}

D3D12_STATIC_SAMPLER_DESC MakeLinearSampler() {
	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ShaderRegister = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	return sampler;
}

json ToJson(const Vector3& value) {
	return json::array({ value.x, value.y, value.z });
}

json ToJson(const Vector4& value) {
	return json::array({ value.x, value.y, value.z, value.w });
}

Vector3 ReadVector3(const json& value, const Vector3& fallback) {
	if (!value.is_array() || value.size() < 3) {
		return fallback;
	}

	return {
		value.at(0).get<float>(),
		value.at(1).get<float>(),
		value.at(2).get<float>()
	};
}

Vector4 ReadVector4(const json& value, const Vector4& fallback) {
	if (!value.is_array() || value.size() < 4) {
		return fallback;
	}

	return {
		value.at(0).get<float>(),
		value.at(1).get<float>(),
		value.at(2).get<float>(),
		value.at(3).get<float>()
	};
}

bool IsCsvPath(const std::string& filePath) {
	const size_t dot = filePath.find_last_of('.');
	if (dot == std::string::npos) {
		return false;
	}

	std::string extension = filePath.substr(dot);
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return extension == ".csv";
}

std::vector<std::string> SplitCsvLine(const std::string& line) {
	std::vector<std::string> values;
	std::stringstream stream(line);
	std::string value;
	while (std::getline(stream, value, ',')) {
		values.push_back(value);
	}
	return values;
}

float ReadCsvFloat(
	const std::unordered_map<std::string, std::vector<std::string>>& table,
	const std::string& key,
	size_t index,
	float fallback
) {
	const auto it = table.find(key);
	if (it == table.end() || it->second.size() <= index + 1) {
		return fallback;
	}

	return std::stof(it->second[index + 1]);
}

uint32_t ReadCsvUint(
	const std::unordered_map<std::string, std::vector<std::string>>& table,
	const std::string& key,
	uint32_t fallback
) {
	const auto it = table.find(key);
	if (it == table.end() || it->second.size() <= 1) {
		return fallback;
	}

	return static_cast<uint32_t>(std::stoul(it->second[1]));
}

Vector3 ReadCsvVector3(
	const std::unordered_map<std::string, std::vector<std::string>>& table,
	const std::string& key,
	const Vector3& fallback
) {
	return {
		ReadCsvFloat(table, key, 0, fallback.x),
		ReadCsvFloat(table, key, 1, fallback.y),
		ReadCsvFloat(table, key, 2, fallback.z)
	};
}

Vector4 ReadCsvVector4(
	const std::unordered_map<std::string, std::vector<std::string>>& table,
	const std::string& key,
	const Vector4& fallback
) {
	return {
		ReadCsvFloat(table, key, 0, fallback.x),
		ReadCsvFloat(table, key, 1, fallback.y),
		ReadCsvFloat(table, key, 2, fallback.z),
		ReadCsvFloat(table, key, 3, fallback.w)
	};
}

} // namespace

bool GpuParticle::Initialize(
	ParticleCommon* particleCommon,
	SrvManager* srvManager,
	const std::string& textureFilePath
) {
	assert(particleCommon);
	assert(srvManager);
	if (!particleCommon || !srvManager || IsInitialized()) {
		return false;
	}

	particleCommon_ = particleCommon;
	srvManager_ = srvManager;
	dxCommon_ = particleCommon_->GetDxCommon();
	config_.textureFilePath = textureFilePath;
	config_.filePath = "resources/particles/gpu_particle.json";
	textureFilePath_ = textureFilePath;

	const bool loaded = ApplyTexture(textureFilePath_);
	if (!loaded) {
		Reset();
		return false;
	}

	if (!CreateParticleResource()) {
		Reset();
		return false;
	}
	CreateConstantBuffers();
	CreateRootSignatures();
	CreatePipelineStates();
	if (!LoadConfig(config_.filePath)) {
		ApplyConfigToGpu();
	}
	CopyStringsToBuffers();

	needsInitialize_ = true;
	return true;
}

void GpuParticle::Reset() {
	particleResource_.Reset();
	freeListIndexResource_.Reset();
	freeListResource_.Reset();
	materialResource_.Reset();
	directionalLightResource_.Reset();
	perViewResource_.Reset();
	emitterResource_.Reset();
	perFrameResource_.Reset();
	behaviorResource_.Reset();
	graphicsRootSignature_.Reset();
	for (auto& pipelineState : graphicsPipelineStates_) {
		pipelineState.Reset();
	}
	initializeRootSignature_.Reset();
	initializePipelineState_.Reset();
	emitRootSignature_.Reset();
	emitPipelineState_.Reset();
	updatePipelineState_.Reset();

	materialData_ = nullptr;
	directionalLightData_ = nullptr;
	perViewData_ = nullptr;
	emitterData_ = nullptr;
	perFrameData_ = nullptr;
	behaviorData_ = nullptr;
	particleCommon_ = nullptr;
	srvManager_ = nullptr;
	dxCommon_ = nullptr;
	textureFilePath_.clear();
	textureSrvIndex_ = UINT32_MAX;
	particleSrvIndex_ = UINT32_MAX;
	particleUavIndex_ = UINT32_MAX;
	freeListIndexUavIndex_ = UINT32_MAX;
	freeListUavIndex_ = UINT32_MAX;
	particleResourceState_ = D3D12_RESOURCE_STATE_COMMON;
	freeListIndexResourceState_ = D3D12_RESOURCE_STATE_COMMON;
	freeListResourceState_ = D3D12_RESOURCE_STATE_COMMON;
	needsInitialize_ = true;
	elapsedTime_ = 0.0f;
	config_ = {};
	std::memset(configPathBuffer_, 0, sizeof(configPathBuffer_));
	std::memset(texturePathBuffer_, 0, sizeof(texturePathBuffer_));
}

void GpuParticle::ClearParticles() {
	needsInitialize_ = true;
	elapsedTime_ = 0.0f;
	if (emitterData_) {
		emitterData_->frequencyTime = 0.0f;
		emitterData_->emit = 0;
	}
	if (perFrameData_) {
		perFrameData_->time = 0.0f;
		perFrameData_->deltaTime = deltaTime_;
	}
}

bool GpuParticle::CreateParticleResource() {
	assert(dxCommon_);
	assert(srvManager_);
	if (!dxCommon_ || !srvManager_) {
		return false;
	}

	std::array<uint32_t, 4> allocatedIndices{
		SrvManager::kInvalidIndex,
		SrvManager::kInvalidIndex,
		SrvManager::kInvalidIndex,
		SrvManager::kInvalidIndex
	};
	auto rollback = [this, &allocatedIndices]() {
		for (uint32_t allocatedIndex : allocatedIndices) {
			if (allocatedIndex != SrvManager::kInvalidIndex) {
				srvManager_->Free(allocatedIndex);
			}
		}
		particleResource_.Reset();
		freeListIndexResource_.Reset();
		freeListResource_.Reset();
		particleSrvIndex_ = SrvManager::kInvalidIndex;
		particleUavIndex_ = SrvManager::kInvalidIndex;
		freeListIndexUavIndex_ = SrvManager::kInvalidIndex;
		freeListUavIndex_ = SrvManager::kInvalidIndex;
		particleResourceState_ = D3D12_RESOURCE_STATE_COMMON;
		freeListIndexResourceState_ = D3D12_RESOURCE_STATE_COMMON;
		freeListResourceState_ = D3D12_RESOURCE_STATE_COMMON;
	};
	for (uint32_t& index : allocatedIndices) {
		if (srvManager_->TryAllocate(index)) {
			continue;
		}
		rollback();
		return false;
	}

	particleResource_ = CreateUavBufferResource(
		dxCommon_->GetDevice(),
		sizeof(ParticleData) * kMaxParticles
	);
	particleResourceState_ = D3D12_RESOURCE_STATE_COMMON;

	if (!srvManager_->CreateSRVforStructuredBuffer(
		allocatedIndices[0],
		particleResource_.Get(),
		kMaxParticles,
		sizeof(ParticleData)
	)) {
		rollback();
		return false;
	}

	if (!srvManager_->CreateUAVforStructuredBuffer(
		allocatedIndices[1],
		particleResource_.Get(),
		kMaxParticles,
		sizeof(ParticleData)
	)) {
		rollback();
		return false;
	}

	freeListIndexResource_ =
		CreateUavBufferResource(dxCommon_->GetDevice(), sizeof(int32_t));
	freeListIndexResourceState_ = D3D12_RESOURCE_STATE_COMMON;

	if (!srvManager_->CreateUAVforStructuredBuffer(
		allocatedIndices[2],
		freeListIndexResource_.Get(),
		1,
		sizeof(int32_t)
	)) {
		rollback();
		return false;
	}

	freeListResource_ = CreateUavBufferResource(
		dxCommon_->GetDevice(),
		sizeof(uint32_t) * kMaxParticles
	);
	freeListResourceState_ = D3D12_RESOURCE_STATE_COMMON;

	if (!srvManager_->CreateUAVforStructuredBuffer(
		allocatedIndices[3],
		freeListResource_.Get(),
		kMaxParticles,
		sizeof(uint32_t)
	)) {
		rollback();
		return false;
	}
	particleSrvIndex_ = allocatedIndices[0];
	particleUavIndex_ = allocatedIndices[1];
	freeListIndexUavIndex_ = allocatedIndices[2];
	freeListUavIndex_ = allocatedIndices[3];
	return true;
}

bool GpuParticle::IsInitialized() const {
	return
		srvManager_ &&
		particleResource_ &&
		freeListIndexResource_ &&
		freeListResource_ &&
		srvManager_->IsAllocated(particleSrvIndex_) &&
		srvManager_->IsAllocated(particleUavIndex_) &&
		srvManager_->IsAllocated(freeListIndexUavIndex_) &&
		srvManager_->IsAllocated(freeListUavIndex_);
}

void GpuParticle::CreateConstantBuffers() {
	materialResource_ = dxCommon_->CreateBufferResource(sizeof(Material));
	materialResource_->Map(0, nullptr, reinterpret_cast<void**>(&materialData_));
	materialData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
	materialData_->enableLighting = false;
	materialData_->alphaCutoff = 0.0f;
	materialData_->flipU = false;
	materialData_->flipV = false;
	materialData_->uvTransform = MakeIdentity4x4();
	materialData_->emissiveIntensity = 1.0f;

	directionalLightResource_ =
		dxCommon_->CreateBufferResource(sizeof(DirectionalLight));
	directionalLightResource_->Map(
		0,
		nullptr,
		reinterpret_cast<void**>(&directionalLightData_)
	);
	directionalLightData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
	directionalLightData_->direction = { 0.0f, -1.0f, 0.0f };
	directionalLightData_->intensity = 1.0f;

	perViewResource_ = dxCommon_->CreateBufferResource(sizeof(PerView));
	perViewResource_->Map(0, nullptr, reinterpret_cast<void**>(&perViewData_));
	std::memset(perViewData_, 0, sizeof(PerView));

	emitterResource_ = dxCommon_->CreateBufferResource(sizeof(EmitterSphere));
	emitterResource_->Map(0, nullptr, reinterpret_cast<void**>(&emitterData_));
	*emitterData_ = config_.emitter;

	perFrameResource_ = dxCommon_->CreateBufferResource(sizeof(PerFrame));
	perFrameResource_->Map(0, nullptr, reinterpret_cast<void**>(&perFrameData_));
	perFrameData_->time = 0.0f;
	perFrameData_->deltaTime = deltaTime_;
	perFrameData_->padding[0] = 0.0f;
	perFrameData_->padding[1] = 0.0f;

	behaviorResource_ = dxCommon_->CreateBufferResource(sizeof(BehaviorForGPU));
	behaviorResource_->Map(0, nullptr, reinterpret_cast<void**>(&behaviorData_));
	ApplyConfigToGpu();
}

void GpuParticle::ApplyConfigToGpu() {
	config_.emitter.frequency = NormalizeEmitterFrequency(config_.emitter.frequency);
	if (emitterData_) {
		const uint32_t emit = emitterData_->emit;
		const float frequencyTime = emitterData_->frequencyTime;
		*emitterData_ = config_.emitter;
		emitterData_->emit = emit;
		emitterData_->frequencyTime = frequencyTime;
	}

	if (behaviorData_) {
		const BehaviorSettings& behavior = config_.behavior;
		behaviorData_->lifeScaleVelocityMinRotationMin = {
			behavior.lifeTimeMin,
			behavior.scaleMin,
			behavior.velocityMin,
			behavior.rotationSpeedMin
		};
		behaviorData_->lifeScaleVelocityMaxRotationMax = {
			behavior.lifeTimeMax,
			behavior.scaleMax,
			behavior.velocityMax,
			behavior.rotationSpeedMax
		};
		behaviorData_->startScaleMin = {
			behavior.startScaleMin.x,
			behavior.startScaleMin.y,
			behavior.startScaleMin.z,
			0.0f
		};
		behaviorData_->startScaleMax = {
			behavior.startScaleMax.x,
			behavior.startScaleMax.y,
			behavior.startScaleMax.z,
			0.0f
		};
		behaviorData_->endScaleMin = {
			behavior.endScaleMin.x,
			behavior.endScaleMin.y,
			behavior.endScaleMin.z,
			0.0f
		};
		behaviorData_->endScaleMax = {
			behavior.endScaleMax.x,
			behavior.endScaleMax.y,
			behavior.endScaleMax.z,
			0.0f
		};
		behaviorData_->velocityBase = {
			behavior.velocityBase.x,
			behavior.velocityBase.y,
			behavior.velocityBase.z,
			0.0f
		};
		behaviorData_->velocityRandomRange = {
			behavior.velocityRandomRange.x,
			behavior.velocityRandomRange.y,
			behavior.velocityRandomRange.z,
			0.0f
		};
		behaviorData_->accelerationBase = {
			behavior.accelerationBase.x,
			behavior.accelerationBase.y,
			behavior.accelerationBase.z,
			0.0f
		};
		behaviorData_->accelerationRandomRange = {
			behavior.accelerationRandomRange.x,
			behavior.accelerationRandomRange.y,
			behavior.accelerationRandomRange.z,
			0.0f
		};
		behaviorData_->flags = {
			behavior.enableLifeFade ? 1.0f : 0.0f,
			behavior.fadeOutStartRatio,
			behavior.enableScaleOverLife ? 1.0f : 0.0f,
			static_cast<float>(behavior.colorMode)
		};
		behaviorData_->rotationFlags = {
			behavior.alignToVelocity ? 1.0f : 0.0f,
			static_cast<float>(behavior.alignAxis),
			0.0f,
			0.0f
		};
		behaviorData_->colorMin = behavior.colorMin;
		behaviorData_->colorMax = behavior.colorMax;
		behaviorData_->endColorMin = behavior.endColorMin;
		behaviorData_->endColorMax = behavior.endColorMax;
		behaviorData_->sway = {
			behavior.swayAmplitude,
			behavior.swayFrequency,
			0.0f,
			0.0f
		};
		behaviorData_->pointFieldFlags = {
			behavior.pointFieldEnabled ? 1.0f : 0.0f,
			behavior.pointFieldRadius,
			behavior.pointFieldFalloff,
			behavior.pointFieldDamping
		};
		behaviorData_->pointFieldCenter = {
			behavior.pointFieldCenter.x,
			behavior.pointFieldCenter.y,
			behavior.pointFieldCenter.z,
			0.0f
		};
		behaviorData_->pointFieldStrengths = {
			behavior.pointFieldAttraction,
			behavior.pointFieldRepulsion,
			behavior.pointFieldOrbit,
			0.0f
		};
		behaviorData_->pointFieldOrbitAxis = {
			behavior.pointFieldOrbitAxis.x,
			behavior.pointFieldOrbitAxis.y,
			behavior.pointFieldOrbitAxis.z,
			0.0f
		};
		behaviorData_->motionFlags = {
			static_cast<float>(behavior.movementMode),
			static_cast<float>(behavior.vortexAxis),
			0.0f,
			0.0f
		};
		behaviorData_->vortexCenter = {
			behavior.vortexCenter.x,
			behavior.vortexCenter.y,
			behavior.vortexCenter.z,
			0.0f
		};
		behaviorData_->vortexAngularInwardSpeed = {
			behavior.vortexAngularSpeedMin,
			behavior.vortexAngularSpeedMax,
			behavior.vortexInwardSpeedMin,
			behavior.vortexInwardSpeedMax
		};
		behaviorData_->vortexVerticalSpeed = {
			behavior.vortexVerticalSpeedMin,
			behavior.vortexVerticalSpeedMax,
			0.0f,
			0.0f
		};
	}
}

bool GpuParticle::ApplyTexture(const std::string& textureFilePath) {
	if (textureFilePath.empty()) {
		return false;
	}

	if (!TextureManager::GetInstance()->LoadTexture(textureFilePath)) {
		return false;
	}

	textureFilePath_ = textureFilePath;
	config_.textureFilePath = textureFilePath;
	textureSrvIndex_ = TextureManager::GetInstance()->GetSrvIndex(textureFilePath_);
	return true;
}

void GpuParticle::ApplyEffectDesc(const ParticleEffectDesc& effect) {
	const std::string currentConfigPath = config_.filePath;
	config_ = Config{};
	config_.filePath = currentConfigPath;
	config_.textureFilePath = effect.textureFilePath;
	ApplyTexture(effect.textureFilePath);
	config_.blendMode = effect.blendMode;
	config_.autoEmit = effect.emitter.isActive;
	config_.useBillboard =
		effect.behavior.render.billboardMode == ParticleManager::BillboardMode::kBillboard;
	config_.forceVisible = false;

	if (materialData_) {
		materialData_->alphaCutoff =
			std::clamp(effect.behavior.render.alphaCutoff, 0.0f, 1.0f);
		materialData_->flipU = effect.behavior.render.flipU ? 1 : 0;
		materialData_->flipV = effect.behavior.render.flipV ? 1 : 0;
		materialData_->emissiveIntensity =
			(std::max)(0.0f, effect.behavior.render.emissiveIntensity);
	}

	config_.emitter.translate = effect.emitter.translate;
	config_.emitter.radius =
		(std::max)(0.0f, MaxComponent(effect.emitter.spawnSize) * 0.5f);
	config_.emitter.spawnSize = effect.emitter.spawnSize;
	config_.emitter.shape = 1;
	config_.emitter.count =
		std::clamp(effect.emitter.count, 0u, kMaxParticles);
	config_.emitter.frequency =
		NormalizeEmitterFrequency(effect.emitter.frequency);

	BehaviorSettings& behavior = config_.behavior;
	behavior.lifeTimeMin =
		(std::max)(0.01f, effect.behavior.life.lifeTimeMin);
	behavior.lifeTimeMax =
		(std::max)(behavior.lifeTimeMin, effect.behavior.life.lifeTimeMax);

	behavior.startScaleMin = effect.behavior.scale.startScaleMin;
	behavior.startScaleMax = effect.behavior.scale.startScaleMax;
	behavior.enableScaleOverLife = effect.behavior.scale.enableScaleOverLife;
	behavior.endScaleMin = effect.behavior.scale.endScaleMin;
	behavior.endScaleMax = effect.behavior.scale.endScaleMax;
	behavior.scaleMin =
		SanitizeScale(MaxComponent(behavior.startScaleMin), behavior.scaleMin);
	behavior.scaleMax =
		(std::max)(behavior.scaleMin, SanitizeScale(MaxComponent(behavior.startScaleMax), behavior.scaleMax));

	behavior.velocityBase = effect.behavior.motion.linear.baseVelocity;
	behavior.velocityRandomRange = effect.behavior.motion.linear.velocityRandomRange;
	behavior.accelerationBase = effect.behavior.motion.linear.enableAcceleration
		? effect.behavior.motion.linear.baseAcceleration
		: Vector3{ 0.0f, 0.0f, 0.0f };
	behavior.accelerationRandomRange = effect.behavior.motion.linear.enableAcceleration
		? effect.behavior.motion.linear.accelerationRandomRange
		: Vector3{ 0.0f, 0.0f, 0.0f };
	if (effect.behavior.motion.wind.enabled) {
		const Vector3 windDirection =
			NormalizeVectorOrZero(effect.behavior.motion.wind.direction);
		if (effect.behavior.motion.wind.smoothVelocity) {
			const float windAcceleration =
				(std::max)(0.0f, effect.behavior.motion.wind.acceleration);
			behavior.accelerationBase.x += windDirection.x * windAcceleration;
			behavior.accelerationBase.y += windDirection.y * windAcceleration;
			behavior.accelerationBase.z += windDirection.z * windAcceleration;
		}
		else {
			const float windStrength =
				(std::max)(0.0f, effect.behavior.motion.wind.strength);
			behavior.velocityBase.x += windDirection.x * windStrength;
			behavior.velocityBase.y += windDirection.y * windStrength;
			behavior.velocityBase.z += windDirection.z * windStrength;
		}
	}
	const bool hasConfiguredVelocity =
		LengthSquared(behavior.velocityBase) > 0.000001f ||
		LengthSquared(behavior.velocityRandomRange) > 0.000001f;
	if (hasConfiguredVelocity) {
		behavior.velocityMin = 1.0f;
		behavior.velocityMax = 1.0f;
	}

	behavior.swayAmplitude = effect.behavior.motion.sway.amplitude;
	behavior.swayFrequency = effect.behavior.motion.sway.frequency;

	const ParticleManager::ParticlePointFieldDesc& pointField =
		effect.behavior.motion.pointField;
	behavior.pointFieldEnabled = pointField.enabled;
	behavior.pointFieldCenter = pointField.useEmitterOffset
		? Vector3{
			effect.emitter.translate.x + pointField.center.x,
			effect.emitter.translate.y + pointField.center.y,
			effect.emitter.translate.z + pointField.center.z
		}
		: pointField.center;
	behavior.pointFieldRadius = (std::max)(0.0f, pointField.radius);
	behavior.pointFieldAttraction = pointField.attractionStrength;
	behavior.pointFieldRepulsion = pointField.repulsionStrength;
	behavior.pointFieldOrbit = pointField.orbitStrength;
	behavior.pointFieldFalloff = (std::max)(0.0f, pointField.falloff);
	behavior.pointFieldDamping = (std::max)(0.0f, pointField.damping);
	behavior.pointFieldOrbitAxis =
		NormalizeVectorOrZero(pointField.orbitAxis);
	if (LengthSquared(behavior.pointFieldOrbitAxis) <= 0.000001f) {
		behavior.pointFieldOrbitAxis = { 0.0f, 1.0f, 0.0f };
	}

	behavior.movementMode =
		static_cast<uint32_t>(effect.behavior.motion.mode);
	const ParticleManager::ParticleVortexDesc& vortex =
		effect.behavior.motion.vortex;
	behavior.vortexCenter = vortex.useEmitterOffset
		? Vector3{
			effect.emitter.translate.x + vortex.center.x,
			effect.emitter.translate.y + vortex.center.y,
			effect.emitter.translate.z + vortex.center.z
		}
		: vortex.center;
	behavior.vortexAxis = static_cast<uint32_t>(vortex.axis);
	behavior.vortexAngularSpeedMin = vortex.angularSpeedMin;
	behavior.vortexAngularSpeedMax =
		(std::max)(vortex.angularSpeedMin, vortex.angularSpeedMax);
	behavior.vortexInwardSpeedMin = vortex.inwardSpeedMin;
	behavior.vortexInwardSpeedMax =
		(std::max)(vortex.inwardSpeedMin, vortex.inwardSpeedMax);
	behavior.vortexVerticalSpeedMin = vortex.verticalSpeedMin;
	behavior.vortexVerticalSpeedMax =
		(std::max)(vortex.verticalSpeedMin, vortex.verticalSpeedMax);

	behavior.rotationSpeedMin = effect.behavior.rotation.rotationSpeed.z;
	behavior.rotationSpeedMax = effect.behavior.rotation.rotationSpeed.z;
	behavior.alignToVelocity = effect.behavior.rotation.alignToVelocity;
	behavior.alignAxis =
		static_cast<uint32_t>(effect.behavior.rotation.alignAxis);
	behavior.enableLifeFade = effect.behavior.life.enableLifeFade;
	behavior.fadeOutStartRatio = effect.behavior.life.fadeOutStartRatio;
	behavior.colorMode =
		effect.behavior.color.mode == ParticleManager::ColorChangeMode::kOverLife ? 1u : 0u;
	behavior.colorMin = effect.behavior.color.startColorMin;
	behavior.colorMax = effect.behavior.color.startColorMax;
	behavior.endColorMin = effect.behavior.color.endColorMin;
	behavior.endColorMax = effect.behavior.color.endColorMax;

	ApplyConfigToGpu();
	CopyStringsToBuffers();
	ClearParticles();
	if (emitterData_ && config_.autoEmit && config_.emitter.count > 0) {
		emitterData_->emit |= kEmitFlagEmitParticles;
	}
}

void GpuParticle::RequestResetBuffer() {
	needsInitialize_ = true;
}

void GpuParticle::EmitOnce() {
	if (emitterData_) {
		emitterData_->emit |= kEmitFlagEmitParticles;
	}
}

GpuParticle::RuntimeInfo GpuParticle::GetRuntimeInfo() const {
	RuntimeInfo info{};
	info.initialized = IsInitialized();
	info.autoEmit = config_.autoEmit;
	info.maxParticles = kMaxParticles;
	info.emitCount = config_.emitter.count;
	if (emitterData_) {
		info.emitFlags = emitterData_->emit;
		info.frequency = emitterData_->frequency;
		info.frequencyTime = emitterData_->frequencyTime;
	}
	else {
		info.frequency = config_.emitter.frequency;
	}
	return info;
}

void GpuParticle::CopyStringsToBuffers() {
	strncpy_s(
		configPathBuffer_,
		sizeof(configPathBuffer_),
		config_.filePath.c_str(),
		_TRUNCATE
	);
	strncpy_s(
		texturePathBuffer_,
		sizeof(texturePathBuffer_),
		config_.textureFilePath.c_str(),
		_TRUNCATE
	);
}

void GpuParticle::RefreshTextureFiles() {
	textureFilePaths_ = ResourceTextureCatalog::Collect();
}

bool GpuParticle::SaveConfig(const std::string& filePath) const {
	if (filePath.empty()) {
		return false;
	}

	std::ostringstream output;

	if (IsCsvPath(filePath)) {
		const BehaviorSettings& behavior = config_.behavior;
		output << std::fixed << std::setprecision(6);
		output << "texture," << config_.textureFilePath << "\n";
		output << "blendMode," << ToString(config_.blendMode) << "\n";
		output << "autoEmit," << (config_.autoEmit ? 1 : 0) << "\n";
		output << "useBillboard," << (config_.useBillboard ? 1 : 0) << "\n";
		output << "forceVisible," << (config_.forceVisible ? 1 : 0) << "\n";
		output << "emitter.translate,"
			 << config_.emitter.translate.x << "," << config_.emitter.translate.y
			 << "," << config_.emitter.translate.z << "\n";
		output << "emitter.radius," << config_.emitter.radius << "\n";
		output << "emitter.spawnSize," << config_.emitter.spawnSize.x << ","
			 << config_.emitter.spawnSize.y << "," << config_.emitter.spawnSize.z << "\n";
		output << "emitter.shape," << config_.emitter.shape << "\n";
		output << "emitter.count," << config_.emitter.count << "\n";
		output << "emitter.frequency," << config_.emitter.frequency << "\n";
		output << "behavior.lifeTime," << behavior.lifeTimeMin << ","
			 << behavior.lifeTimeMax << "\n";
		output << "behavior.scale," << behavior.scaleMin << ","
			 << behavior.scaleMax << "\n";
		output << "behavior.startScaleMin," << behavior.startScaleMin.x << ","
			 << behavior.startScaleMin.y << "," << behavior.startScaleMin.z << "\n";
		output << "behavior.startScaleMax," << behavior.startScaleMax.x << ","
			 << behavior.startScaleMax.y << "," << behavior.startScaleMax.z << "\n";
		output << "behavior.enableScaleOverLife,"
			 << (behavior.enableScaleOverLife ? 1 : 0) << "\n";
		output << "behavior.endScaleMin," << behavior.endScaleMin.x << ","
			 << behavior.endScaleMin.y << "," << behavior.endScaleMin.z << "\n";
		output << "behavior.endScaleMax," << behavior.endScaleMax.x << ","
			 << behavior.endScaleMax.y << "," << behavior.endScaleMax.z << "\n";
		output << "behavior.velocity," << behavior.velocityMin << ","
			 << behavior.velocityMax << "\n";
		output << "behavior.velocityBase," << behavior.velocityBase.x << ","
			 << behavior.velocityBase.y << "," << behavior.velocityBase.z << "\n";
		output << "behavior.velocityRandomRange,"
			 << behavior.velocityRandomRange.x << ","
			 << behavior.velocityRandomRange.y << ","
			 << behavior.velocityRandomRange.z << "\n";
		output << "behavior.accelerationBase," << behavior.accelerationBase.x << ","
			 << behavior.accelerationBase.y << "," << behavior.accelerationBase.z << "\n";
		output << "behavior.accelerationRandomRange,"
			 << behavior.accelerationRandomRange.x << ","
			 << behavior.accelerationRandomRange.y << ","
			 << behavior.accelerationRandomRange.z << "\n";
		output << "behavior.rotationSpeed," << behavior.rotationSpeedMin << ","
			 << behavior.rotationSpeedMax << "\n";
		output << "behavior.alignToVelocity," << (behavior.alignToVelocity ? 1 : 0) << "\n";
		output << "behavior.alignAxis," << behavior.alignAxis << "\n";
		output << "behavior.enableLifeFade," << (behavior.enableLifeFade ? 1 : 0) << "\n";
		output << "behavior.fadeOutStartRatio," << behavior.fadeOutStartRatio << "\n";
		output << "behavior.colorMode," << behavior.colorMode << "\n";
		output << "behavior.colorMin," << behavior.colorMin.x << ","
			 << behavior.colorMin.y << "," << behavior.colorMin.z << ","
			 << behavior.colorMin.w << "\n";
		output << "behavior.colorMax," << behavior.colorMax.x << ","
			 << behavior.colorMax.y << "," << behavior.colorMax.z << ","
			 << behavior.colorMax.w << "\n";
		output << "behavior.endColorMin," << behavior.endColorMin.x << ","
			 << behavior.endColorMin.y << "," << behavior.endColorMin.z << ","
			 << behavior.endColorMin.w << "\n";
		output << "behavior.endColorMax," << behavior.endColorMax.x << ","
			 << behavior.endColorMax.y << "," << behavior.endColorMax.z << ","
			 << behavior.endColorMax.w << "\n";
		return EditableResourcePath::WriteTextAtomically(filePath, output.str());
	}

	json root;
	root["texture"] = config_.textureFilePath;
	root["blendMode"] = ToString(config_.blendMode);
	root["autoEmit"] = config_.autoEmit;
	root["useBillboard"] = config_.useBillboard;
	root["forceVisible"] = config_.forceVisible;
	root["emitter"] = {
		{ "translate", ToJson(config_.emitter.translate) },
		{ "radius", config_.emitter.radius },
		{ "spawnSize", ToJson(config_.emitter.spawnSize) },
		{ "shape", config_.emitter.shape },
		{ "count", config_.emitter.count },
		{ "frequency", config_.emitter.frequency }
	};

	const BehaviorSettings& behavior = config_.behavior;
	root["behavior"] = {
		{ "lifeTime", json::array({ behavior.lifeTimeMin, behavior.lifeTimeMax }) },
		{ "scale", json::array({ behavior.scaleMin, behavior.scaleMax }) },
		{ "startScaleMin", ToJson(behavior.startScaleMin) },
		{ "startScaleMax", ToJson(behavior.startScaleMax) },
		{ "enableScaleOverLife", behavior.enableScaleOverLife },
		{ "endScaleMin", ToJson(behavior.endScaleMin) },
		{ "endScaleMax", ToJson(behavior.endScaleMax) },
		{ "velocity", json::array({ behavior.velocityMin, behavior.velocityMax }) },
		{ "velocityBase", ToJson(behavior.velocityBase) },
		{ "velocityRandomRange", ToJson(behavior.velocityRandomRange) },
		{ "accelerationBase", ToJson(behavior.accelerationBase) },
		{ "accelerationRandomRange", ToJson(behavior.accelerationRandomRange) },
		{ "swayAmplitude", behavior.swayAmplitude },
		{ "swayFrequency", behavior.swayFrequency },
		{ "pointFieldEnabled", behavior.pointFieldEnabled },
		{ "pointFieldCenter", ToJson(behavior.pointFieldCenter) },
		{ "pointFieldRadius", behavior.pointFieldRadius },
		{ "pointFieldAttraction", behavior.pointFieldAttraction },
		{ "pointFieldRepulsion", behavior.pointFieldRepulsion },
		{ "pointFieldOrbit", behavior.pointFieldOrbit },
		{ "pointFieldOrbitAxis", ToJson(behavior.pointFieldOrbitAxis) },
		{ "pointFieldFalloff", behavior.pointFieldFalloff },
		{ "pointFieldDamping", behavior.pointFieldDamping },
		{ "movementMode", behavior.movementMode },
		{ "vortexCenter", ToJson(behavior.vortexCenter) },
		{ "vortexAxis", behavior.vortexAxis },
		{
			"vortexAngularSpeed",
			json::array({
				behavior.vortexAngularSpeedMin,
				behavior.vortexAngularSpeedMax
			})
		},
		{
			"vortexInwardSpeed",
			json::array({
				behavior.vortexInwardSpeedMin,
				behavior.vortexInwardSpeedMax
			})
		},
		{
			"vortexVerticalSpeed",
			json::array({
				behavior.vortexVerticalSpeedMin,
				behavior.vortexVerticalSpeedMax
			})
		},
		{
			"rotationSpeed",
			json::array({ behavior.rotationSpeedMin, behavior.rotationSpeedMax })
		},
		{ "alignToVelocity", behavior.alignToVelocity },
		{ "alignAxis", behavior.alignAxis },
		{ "enableLifeFade", behavior.enableLifeFade },
		{ "fadeOutStartRatio", behavior.fadeOutStartRatio },
		{ "colorMode", behavior.colorMode },
		{ "colorMin", ToJson(behavior.colorMin) },
		{ "colorMax", ToJson(behavior.colorMax) },
		{ "endColorMin", ToJson(behavior.endColorMin) },
		{ "endColorMax", ToJson(behavior.endColorMax) }
	};

	output << std::setw(4) << root << '\n';
	return EditableResourcePath::WriteTextAtomically(filePath, output.str());
}

bool GpuParticle::LoadConfig(const std::string& filePath) {
	if (filePath.empty()) {
		return false;
	}

	const std::filesystem::path resolvedPath = EditableResourcePath::Resolve(filePath);
	std::string configText;
	if (!EditableResourcePath::ReadText(filePath, configText)) {
		return false;
	}
	if (!IsCsvPath(filePath)) {
		try {
			(void)json::parse(configText);
		} catch (...) {
			if (!EditableResourcePath::ReadTextFile(
				EditableResourcePath::BackupPath(resolvedPath),
				configText
			)) {
				return false;
			}
		}
	}
	std::istringstream file(configText);

	Config loaded = config_;
	loaded.filePath = filePath;

	try {
		if (IsCsvPath(filePath)) {
			std::unordered_map<std::string, std::vector<std::string>> table;
			std::string line;
			while (std::getline(file, line)) {
				std::vector<std::string> values = SplitCsvLine(line);
				if (!values.empty()) {
					table.emplace(values.front(), std::move(values));
				}
			}

			if (const auto it = table.find("texture"); it != table.end() &&
				it->second.size() > 1) {
				loaded.textureFilePath = it->second[1];
			}
			if (const auto it = table.find("blendMode"); it != table.end() &&
				it->second.size() > 1) {
				loaded.blendMode = ToBlendMode(it->second[1]);
			}
			loaded.autoEmit = ReadCsvUint(table, "autoEmit", loaded.autoEmit ? 1u : 0u) != 0;
			loaded.useBillboard =
				ReadCsvUint(table, "useBillboard", loaded.useBillboard ? 1u : 0u) != 0;
			loaded.forceVisible =
				ReadCsvUint(table, "forceVisible", loaded.forceVisible ? 1u : 0u) != 0;
			loaded.emitter.translate =
				ReadCsvVector3(table, "emitter.translate", loaded.emitter.translate);
			loaded.emitter.radius =
				ReadCsvFloat(table, "emitter.radius", 0, loaded.emitter.radius);
			loaded.emitter.spawnSize =
				ReadCsvVector3(table, "emitter.spawnSize", loaded.emitter.spawnSize);
			loaded.emitter.shape =
				ReadCsvUint(table, "emitter.shape", loaded.emitter.shape);
			loaded.emitter.count =
				ReadCsvUint(table, "emitter.count", loaded.emitter.count);
			loaded.emitter.frequency =
				ReadCsvFloat(table, "emitter.frequency", 0, loaded.emitter.frequency);
			loaded.behavior.lifeTimeMin =
				ReadCsvFloat(table, "behavior.lifeTime", 0, loaded.behavior.lifeTimeMin);
			loaded.behavior.lifeTimeMax =
				ReadCsvFloat(table, "behavior.lifeTime", 1, loaded.behavior.lifeTimeMax);
			loaded.behavior.scaleMin =
				ReadCsvFloat(table, "behavior.scale", 0, loaded.behavior.scaleMin);
			loaded.behavior.scaleMax =
				ReadCsvFloat(table, "behavior.scale", 1, loaded.behavior.scaleMax);
			loaded.behavior.startScaleMin =
				ReadCsvVector3(table, "behavior.startScaleMin", loaded.behavior.startScaleMin);
			loaded.behavior.startScaleMax =
				ReadCsvVector3(table, "behavior.startScaleMax", loaded.behavior.startScaleMax);
			loaded.behavior.enableScaleOverLife =
				ReadCsvUint(table, "behavior.enableScaleOverLife", loaded.behavior.enableScaleOverLife ? 1u : 0u) != 0;
			loaded.behavior.endScaleMin =
				ReadCsvVector3(table, "behavior.endScaleMin", loaded.behavior.endScaleMin);
			loaded.behavior.endScaleMax =
				ReadCsvVector3(table, "behavior.endScaleMax", loaded.behavior.endScaleMax);
			loaded.behavior.velocityMin =
				ReadCsvFloat(table, "behavior.velocity", 0, loaded.behavior.velocityMin);
			loaded.behavior.velocityMax =
				ReadCsvFloat(table, "behavior.velocity", 1, loaded.behavior.velocityMax);
			loaded.behavior.velocityBase =
				ReadCsvVector3(table, "behavior.velocityBase", loaded.behavior.velocityBase);
			loaded.behavior.velocityRandomRange = ReadCsvVector3(
				table,
				"behavior.velocityRandomRange",
				loaded.behavior.velocityRandomRange
			);
			loaded.behavior.accelerationBase =
				ReadCsvVector3(table, "behavior.accelerationBase", loaded.behavior.accelerationBase);
			loaded.behavior.accelerationRandomRange = ReadCsvVector3(
				table,
				"behavior.accelerationRandomRange",
				loaded.behavior.accelerationRandomRange
			);
			loaded.behavior.swayAmplitude =
				ReadCsvFloat(table, "behavior.sway", 0, loaded.behavior.swayAmplitude);
			loaded.behavior.swayFrequency =
				ReadCsvFloat(table, "behavior.sway", 1, loaded.behavior.swayFrequency);
			loaded.behavior.pointFieldEnabled =
				ReadCsvUint(table, "behavior.pointFieldEnabled", loaded.behavior.pointFieldEnabled ? 1u : 0u) != 0;
			loaded.behavior.pointFieldCenter =
				ReadCsvVector3(table, "behavior.pointFieldCenter", loaded.behavior.pointFieldCenter);
			loaded.behavior.pointFieldRadius =
				ReadCsvFloat(table, "behavior.pointFieldRadius", 0, loaded.behavior.pointFieldRadius);
			loaded.behavior.pointFieldAttraction =
				ReadCsvFloat(table, "behavior.pointFieldAttraction", 0, loaded.behavior.pointFieldAttraction);
			loaded.behavior.pointFieldRepulsion =
				ReadCsvFloat(table, "behavior.pointFieldRepulsion", 0, loaded.behavior.pointFieldRepulsion);
			loaded.behavior.pointFieldOrbit =
				ReadCsvFloat(table, "behavior.pointFieldOrbit", 0, loaded.behavior.pointFieldOrbit);
			loaded.behavior.pointFieldOrbitAxis =
				ReadCsvVector3(table, "behavior.pointFieldOrbitAxis", loaded.behavior.pointFieldOrbitAxis);
			loaded.behavior.pointFieldFalloff =
				ReadCsvFloat(table, "behavior.pointFieldFalloff", 0, loaded.behavior.pointFieldFalloff);
			loaded.behavior.pointFieldDamping =
				ReadCsvFloat(table, "behavior.pointFieldDamping", 0, loaded.behavior.pointFieldDamping);
			loaded.behavior.movementMode =
				ReadCsvUint(table, "behavior.movementMode", loaded.behavior.movementMode);
			loaded.behavior.vortexCenter =
				ReadCsvVector3(table, "behavior.vortexCenter", loaded.behavior.vortexCenter);
			loaded.behavior.vortexAxis =
				ReadCsvUint(table, "behavior.vortexAxis", loaded.behavior.vortexAxis);
			loaded.behavior.vortexAngularSpeedMin = ReadCsvFloat(
				table,
				"behavior.vortexAngularSpeed",
				0,
				loaded.behavior.vortexAngularSpeedMin
			);
			loaded.behavior.vortexAngularSpeedMax = ReadCsvFloat(
				table,
				"behavior.vortexAngularSpeed",
				1,
				loaded.behavior.vortexAngularSpeedMax
			);
			loaded.behavior.vortexInwardSpeedMin = ReadCsvFloat(
				table,
				"behavior.vortexInwardSpeed",
				0,
				loaded.behavior.vortexInwardSpeedMin
			);
			loaded.behavior.vortexInwardSpeedMax = ReadCsvFloat(
				table,
				"behavior.vortexInwardSpeed",
				1,
				loaded.behavior.vortexInwardSpeedMax
			);
			loaded.behavior.vortexVerticalSpeedMin = ReadCsvFloat(
				table,
				"behavior.vortexVerticalSpeed",
				0,
				loaded.behavior.vortexVerticalSpeedMin
			);
			loaded.behavior.vortexVerticalSpeedMax = ReadCsvFloat(
				table,
				"behavior.vortexVerticalSpeed",
				1,
				loaded.behavior.vortexVerticalSpeedMax
			);
			loaded.behavior.rotationSpeedMin = ReadCsvFloat(
				table,
				"behavior.rotationSpeed",
				0,
				loaded.behavior.rotationSpeedMin
			);
			loaded.behavior.rotationSpeedMax = ReadCsvFloat(
				table,
				"behavior.rotationSpeed",
				1,
				loaded.behavior.rotationSpeedMax
			);
			loaded.behavior.alignToVelocity =
				ReadCsvUint(table, "behavior.alignToVelocity", loaded.behavior.alignToVelocity ? 1u : 0u) != 0;
			loaded.behavior.alignAxis =
				ReadCsvUint(table, "behavior.alignAxis", loaded.behavior.alignAxis);
			loaded.behavior.enableLifeFade =
				ReadCsvUint(table, "behavior.enableLifeFade", loaded.behavior.enableLifeFade ? 1u : 0u) != 0;
			loaded.behavior.fadeOutStartRatio =
				ReadCsvFloat(table, "behavior.fadeOutStartRatio", 0, loaded.behavior.fadeOutStartRatio);
			loaded.behavior.colorMode =
				ReadCsvUint(table, "behavior.colorMode", loaded.behavior.colorMode);
			loaded.behavior.colorMin =
				ReadCsvVector4(table, "behavior.colorMin", loaded.behavior.colorMin);
			loaded.behavior.colorMax =
				ReadCsvVector4(table, "behavior.colorMax", loaded.behavior.colorMax);
			loaded.behavior.endColorMin =
				ReadCsvVector4(table, "behavior.endColorMin", loaded.behavior.endColorMin);
			loaded.behavior.endColorMax =
				ReadCsvVector4(table, "behavior.endColorMax", loaded.behavior.endColorMax);
		}
		else {
			json root;
			file >> root;

			loaded.textureFilePath = root.value("texture", loaded.textureFilePath);
			loaded.blendMode =
				ToBlendMode(root.value("blendMode", std::string(ToString(loaded.blendMode))));
			loaded.autoEmit = root.value("autoEmit", loaded.autoEmit);
			loaded.useBillboard = root.value("useBillboard", loaded.useBillboard);
			loaded.forceVisible = root.value("forceVisible", loaded.forceVisible);

			if (root.contains("emitter")) {
				const json& emitter = root.at("emitter");
				if (emitter.contains("translate")) {
					loaded.emitter.translate =
						ReadVector3(emitter.at("translate"), loaded.emitter.translate);
				}
				loaded.emitter.radius = emitter.value("radius", loaded.emitter.radius);
				if (emitter.contains("spawnSize")) {
					loaded.emitter.spawnSize =
						ReadVector3(emitter.at("spawnSize"), loaded.emitter.spawnSize);
				}
				loaded.emitter.shape = emitter.value("shape", loaded.emitter.shape);
				loaded.emitter.count = emitter.value("count", loaded.emitter.count);
				loaded.emitter.frequency =
					emitter.value("frequency", loaded.emitter.frequency);
			}

			if (root.contains("behavior")) {
				const json& behavior = root.at("behavior");
				if (behavior.contains("lifeTime") &&
					behavior.at("lifeTime").is_array() &&
					behavior.at("lifeTime").size() >= 2) {
					loaded.behavior.lifeTimeMin =
						behavior.at("lifeTime").at(0).get<float>();
					loaded.behavior.lifeTimeMax =
						behavior.at("lifeTime").at(1).get<float>();
				}
				if (behavior.contains("scale") && behavior.at("scale").is_array() &&
					behavior.at("scale").size() >= 2) {
					loaded.behavior.scaleMin = behavior.at("scale").at(0).get<float>();
					loaded.behavior.scaleMax = behavior.at("scale").at(1).get<float>();
				}
				if (behavior.contains("startScaleMin")) {
					loaded.behavior.startScaleMin =
						ReadVector3(behavior.at("startScaleMin"), loaded.behavior.startScaleMin);
				}
				if (behavior.contains("startScaleMax")) {
					loaded.behavior.startScaleMax =
						ReadVector3(behavior.at("startScaleMax"), loaded.behavior.startScaleMax);
				}
				loaded.behavior.enableScaleOverLife =
					behavior.value("enableScaleOverLife", loaded.behavior.enableScaleOverLife);
				if (behavior.contains("endScaleMin")) {
					loaded.behavior.endScaleMin =
						ReadVector3(behavior.at("endScaleMin"), loaded.behavior.endScaleMin);
				}
				if (behavior.contains("endScaleMax")) {
					loaded.behavior.endScaleMax =
						ReadVector3(behavior.at("endScaleMax"), loaded.behavior.endScaleMax);
				}
				if (behavior.contains("velocity") &&
					behavior.at("velocity").is_array() &&
					behavior.at("velocity").size() >= 2) {
					loaded.behavior.velocityMin =
						behavior.at("velocity").at(0).get<float>();
					loaded.behavior.velocityMax =
						behavior.at("velocity").at(1).get<float>();
				}
				if (behavior.contains("velocityBase")) {
					loaded.behavior.velocityBase = ReadVector3(
						behavior.at("velocityBase"),
						loaded.behavior.velocityBase
					);
				}
				if (behavior.contains("velocityRandomRange")) {
					loaded.behavior.velocityRandomRange = ReadVector3(
						behavior.at("velocityRandomRange"),
						loaded.behavior.velocityRandomRange
					);
				}
				if (behavior.contains("accelerationBase")) {
					loaded.behavior.accelerationBase = ReadVector3(
						behavior.at("accelerationBase"),
						loaded.behavior.accelerationBase
					);
				}
				if (behavior.contains("accelerationRandomRange")) {
					loaded.behavior.accelerationRandomRange = ReadVector3(
						behavior.at("accelerationRandomRange"),
						loaded.behavior.accelerationRandomRange
					);
				}
				loaded.behavior.swayAmplitude =
					behavior.value("swayAmplitude", loaded.behavior.swayAmplitude);
				loaded.behavior.swayFrequency =
					behavior.value("swayFrequency", loaded.behavior.swayFrequency);
				loaded.behavior.pointFieldEnabled =
					behavior.value("pointFieldEnabled", loaded.behavior.pointFieldEnabled);
				if (behavior.contains("pointFieldCenter")) {
					loaded.behavior.pointFieldCenter = ReadVector3(
						behavior.at("pointFieldCenter"),
						loaded.behavior.pointFieldCenter
					);
				}
				loaded.behavior.pointFieldRadius =
					behavior.value("pointFieldRadius", loaded.behavior.pointFieldRadius);
				loaded.behavior.pointFieldAttraction =
					behavior.value("pointFieldAttraction", loaded.behavior.pointFieldAttraction);
				loaded.behavior.pointFieldRepulsion =
					behavior.value("pointFieldRepulsion", loaded.behavior.pointFieldRepulsion);
				loaded.behavior.pointFieldOrbit =
					behavior.value("pointFieldOrbit", loaded.behavior.pointFieldOrbit);
				if (behavior.contains("pointFieldOrbitAxis")) {
					loaded.behavior.pointFieldOrbitAxis = ReadVector3(
						behavior.at("pointFieldOrbitAxis"),
						loaded.behavior.pointFieldOrbitAxis
					);
				}
				loaded.behavior.pointFieldFalloff =
					behavior.value("pointFieldFalloff", loaded.behavior.pointFieldFalloff);
				loaded.behavior.pointFieldDamping =
					behavior.value("pointFieldDamping", loaded.behavior.pointFieldDamping);
				loaded.behavior.movementMode =
					behavior.value("movementMode", loaded.behavior.movementMode);
				if (behavior.contains("vortexCenter")) {
					loaded.behavior.vortexCenter = ReadVector3(
						behavior.at("vortexCenter"),
						loaded.behavior.vortexCenter
					);
				}
				loaded.behavior.vortexAxis =
					behavior.value("vortexAxis", loaded.behavior.vortexAxis);
				if (behavior.contains("vortexAngularSpeed") &&
					behavior.at("vortexAngularSpeed").is_array() &&
					behavior.at("vortexAngularSpeed").size() >= 2) {
					loaded.behavior.vortexAngularSpeedMin =
						behavior.at("vortexAngularSpeed").at(0).get<float>();
					loaded.behavior.vortexAngularSpeedMax =
						behavior.at("vortexAngularSpeed").at(1).get<float>();
				}
				if (behavior.contains("vortexInwardSpeed") &&
					behavior.at("vortexInwardSpeed").is_array() &&
					behavior.at("vortexInwardSpeed").size() >= 2) {
					loaded.behavior.vortexInwardSpeedMin =
						behavior.at("vortexInwardSpeed").at(0).get<float>();
					loaded.behavior.vortexInwardSpeedMax =
						behavior.at("vortexInwardSpeed").at(1).get<float>();
				}
				if (behavior.contains("vortexVerticalSpeed") &&
					behavior.at("vortexVerticalSpeed").is_array() &&
					behavior.at("vortexVerticalSpeed").size() >= 2) {
					loaded.behavior.vortexVerticalSpeedMin =
						behavior.at("vortexVerticalSpeed").at(0).get<float>();
					loaded.behavior.vortexVerticalSpeedMax =
						behavior.at("vortexVerticalSpeed").at(1).get<float>();
				}
				if (behavior.contains("rotationSpeed") &&
					behavior.at("rotationSpeed").is_array() &&
					behavior.at("rotationSpeed").size() >= 2) {
					loaded.behavior.rotationSpeedMin =
						behavior.at("rotationSpeed").at(0).get<float>();
					loaded.behavior.rotationSpeedMax =
						behavior.at("rotationSpeed").at(1).get<float>();
				}
				loaded.behavior.alignToVelocity =
					behavior.value("alignToVelocity", loaded.behavior.alignToVelocity);
				loaded.behavior.alignAxis =
					behavior.value("alignAxis", loaded.behavior.alignAxis);
				loaded.behavior.enableLifeFade =
					behavior.value("enableLifeFade", loaded.behavior.enableLifeFade);
				loaded.behavior.fadeOutStartRatio =
					behavior.value("fadeOutStartRatio", loaded.behavior.fadeOutStartRatio);
				loaded.behavior.colorMode =
					behavior.value("colorMode", loaded.behavior.colorMode);
				if (behavior.contains("colorMin")) {
					loaded.behavior.colorMin =
						ReadVector4(behavior.at("colorMin"), loaded.behavior.colorMin);
				}
				if (behavior.contains("colorMax")) {
					loaded.behavior.colorMax =
						ReadVector4(behavior.at("colorMax"), loaded.behavior.colorMax);
				}
				if (behavior.contains("endColorMin")) {
					loaded.behavior.endColorMin =
						ReadVector4(behavior.at("endColorMin"), loaded.behavior.endColorMin);
				}
				if (behavior.contains("endColorMax")) {
					loaded.behavior.endColorMax =
						ReadVector4(behavior.at("endColorMax"), loaded.behavior.endColorMax);
				}
			}
		}
	}
	catch (...) {
		return false;
	}

	loaded.emitter.count = std::clamp(loaded.emitter.count, 0u, kMaxParticles);
	loaded.emitter.radius = (std::max)(0.0f, loaded.emitter.radius);
	loaded.emitter.shape = std::clamp(loaded.emitter.shape, 0u, 1u);
	loaded.emitter.frequency = NormalizeEmitterFrequency(loaded.emitter.frequency);
	loaded.behavior.lifeTimeMax =
		(std::max)(loaded.behavior.lifeTimeMin, loaded.behavior.lifeTimeMax);
	loaded.behavior.scaleMax =
		(std::max)(loaded.behavior.scaleMin, loaded.behavior.scaleMax);
	loaded.behavior.velocityMax =
		(std::max)(loaded.behavior.velocityMin, loaded.behavior.velocityMax);
	loaded.behavior.fadeOutStartRatio =
		std::clamp(loaded.behavior.fadeOutStartRatio, 0.0f, 1.0f);
	loaded.behavior.colorMode = std::clamp(loaded.behavior.colorMode, 0u, 1u);
	loaded.behavior.alignAxis = std::clamp(loaded.behavior.alignAxis, 0u, 2u);
	loaded.behavior.rotationSpeedMax = (std::max)(
		loaded.behavior.rotationSpeedMin,
		loaded.behavior.rotationSpeedMax
	);
	loaded.behavior.swayAmplitude =
		(std::max)(0.0f, loaded.behavior.swayAmplitude);
	loaded.behavior.swayFrequency =
		(std::max)(0.0f, loaded.behavior.swayFrequency);
	loaded.behavior.pointFieldRadius =
		(std::max)(0.0f, loaded.behavior.pointFieldRadius);
	loaded.behavior.pointFieldFalloff =
		(std::max)(0.0f, loaded.behavior.pointFieldFalloff);
	loaded.behavior.pointFieldDamping =
		(std::max)(0.0f, loaded.behavior.pointFieldDamping);
	loaded.behavior.pointFieldOrbitAxis =
		NormalizeVectorOrZero(loaded.behavior.pointFieldOrbitAxis);
	if (LengthSquared(loaded.behavior.pointFieldOrbitAxis) <= 0.000001f) {
		loaded.behavior.pointFieldOrbitAxis = { 0.0f, 1.0f, 0.0f };
	}
	loaded.behavior.movementMode =
		std::clamp(loaded.behavior.movementMode, 0u, 1u);
	loaded.behavior.vortexAxis =
		std::clamp(loaded.behavior.vortexAxis, 0u, 2u);
	loaded.behavior.vortexAngularSpeedMax = (std::max)(
		loaded.behavior.vortexAngularSpeedMin,
		loaded.behavior.vortexAngularSpeedMax
	);
	loaded.behavior.vortexInwardSpeedMax = (std::max)(
		loaded.behavior.vortexInwardSpeedMin,
		loaded.behavior.vortexInwardSpeedMax
	);
	loaded.behavior.vortexVerticalSpeedMax = (std::max)(
		loaded.behavior.vortexVerticalSpeedMin,
		loaded.behavior.vortexVerticalSpeedMax
	);

	config_ = loaded;
	ApplyTexture(config_.textureFilePath);
	ApplyConfigToGpu();
	CopyStringsToBuffers();
	return true;
}

void GpuParticle::DrawImGui(const char* windowTitle) {
#if defined(_DEBUG) || defined(DEVELOPMENT)
	if (!ImGui::Begin(windowTitle)) {
		ImGui::End();
		return;
	}

	bool dirty = false;
	ImGui::InputText("Config Path", configPathBuffer_, sizeof(configPathBuffer_));
	if (ImGui::Button("Save")) {
		config_.filePath = configPathBuffer_;
		SaveConfig(config_.filePath);
	}
	ImGui::SameLine();
	if (ImGui::Button("Load")) {
		LoadConfig(configPathBuffer_);
	}
	ImGui::SameLine();
	if (ImGui::Button("Reset GPU Buffer")) {
		needsInitialize_ = true;
	}

	if (ImGui::InputText("Texture", texturePathBuffer_, sizeof(texturePathBuffer_))) {
		config_.textureFilePath = texturePathBuffer_;
	}
	ImGui::SameLine();
	if (ImGui::Button("Apply Texture")) {
		ApplyTexture(texturePathBuffer_);
		CopyStringsToBuffers();
	}
	if (textureFilePaths_.empty()) RefreshTextureFiles();
	const char* selectedTexture = texturePathBuffer_[0] != '\0'
		? texturePathBuffer_
		: "(select texture)";
	if (ImGui::BeginCombo("Resource Texture", selectedTexture)) {
		for (const std::string& texturePath : textureFilePaths_) {
			const bool selected = texturePath == texturePathBuffer_;
			if (ImGui::Selectable(texturePath.c_str(), selected)) {
				strncpy_s(texturePathBuffer_, sizeof(texturePathBuffer_), texturePath.c_str(), _TRUNCATE);
				config_.textureFilePath = texturePath;
				ApplyTexture(texturePath);
				CopyStringsToBuffers();
			}
			if (selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::Button("Refresh Textures")) RefreshTextureFiles();

	dirty |= ImGui::Checkbox("Auto Emit", &config_.autoEmit);
	ImGui::TextDisabled(
		"GPU: %s / Count: %u / EmitFlags: 0x%X / FreqTime: %.3f",
		IsInitialized() ? "Initialized" : "Not initialized",
		config_.emitter.count,
		emitterData_ ? emitterData_->emit : 0u,
		emitterData_ ? emitterData_->frequencyTime : 0.0f
	);
	ImGui::Checkbox("Use Billboard", &config_.useBillboard);
	ImGui::SameLine();
	ImGui::Checkbox("Force Visible Debug", &config_.forceVisible);
	if (ImGui::Button("Apply Rain Effect")) {
		ParticleEffectDesc rainEffect{};
		if (ParticleEffectResource::Load(
			"resources/particles/rainParticle.json",
			rainEffect
		)) {
			ApplyEffectDesc(rainEffect);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Set Visible Test")) {
		config_.autoEmit = false;
		config_.useBillboard = true;
		config_.forceVisible = false;
		config_.textureFilePath = "resources/circle.png";
		ApplyTexture(config_.textureFilePath);
		config_.blendMode = ParticleCommon::BlendMode::kBlendModeAdd;
		config_.emitter.translate = { 0.0f, 2.0f, 0.0f };
		config_.emitter.radius = 0.0f;
		config_.emitter.spawnSize = { 0.0f, 0.0f, 0.0f };
		config_.emitter.shape = 0;
		config_.emitter.count = 1;
		config_.emitter.frequency = 0.05f;
		config_.behavior.lifeTimeMin = 600.0f;
		config_.behavior.lifeTimeMax = 600.0f;
		config_.behavior.startScaleMin = { 1.0f, 1.0f, 1.0f };
		config_.behavior.startScaleMax = { 1.0f, 1.0f, 1.0f };
		config_.behavior.enableScaleOverLife = false;
		config_.behavior.velocityMin = 0.0f;
		config_.behavior.velocityMax = 0.0f;
		config_.behavior.velocityBase = { 0.0f, 0.0f, 0.0f };
		config_.behavior.velocityRandomRange = { 0.0f, 0.0f, 0.0f };
		config_.behavior.accelerationBase = { 0.0f, 0.0f, 0.0f };
		config_.behavior.accelerationRandomRange = { 0.0f, 0.0f, 0.0f };
		config_.behavior.swayAmplitude = 0.0f;
		config_.behavior.swayFrequency = 0.0f;
		config_.behavior.pointFieldEnabled = false;
		config_.behavior.pointFieldCenter = { 0.0f, 0.0f, 0.0f };
		config_.behavior.pointFieldRadius = 0.0f;
		config_.behavior.pointFieldAttraction = 0.0f;
		config_.behavior.pointFieldRepulsion = 0.0f;
		config_.behavior.pointFieldOrbit = 0.0f;
		config_.behavior.pointFieldOrbitAxis = { 0.0f, 1.0f, 0.0f };
		config_.behavior.pointFieldFalloff = 1.0f;
		config_.behavior.pointFieldDamping = 0.0f;
		config_.behavior.movementMode = 0;
		config_.behavior.vortexCenter = { 0.0f, 0.0f, 0.0f };
		config_.behavior.vortexAxis = 1;
		config_.behavior.vortexAngularSpeedMin = 4.0f;
		config_.behavior.vortexAngularSpeedMax = 8.0f;
		config_.behavior.vortexInwardSpeedMin = 0.8f;
		config_.behavior.vortexInwardSpeedMax = 1.8f;
		config_.behavior.vortexVerticalSpeedMin = -0.1f;
		config_.behavior.vortexVerticalSpeedMax = 0.1f;
		config_.behavior.alignToVelocity = false;
		config_.behavior.alignAxis = 1;
		config_.behavior.enableLifeFade = false;
		config_.behavior.fadeOutStartRatio = 1.0f;
		config_.behavior.colorMode = 0;
		config_.behavior.colorMin = { 4.0f, 0.2f, 0.2f, 1.0f };
		config_.behavior.colorMax = { 8.0f, 0.6f, 0.2f, 1.0f };
		ApplyConfigToGpu();
		CopyStringsToBuffers();
		ClearParticles();
		if (emitterData_) {
			emitterData_->emit = kEmitFlagSeedVisibleParticle;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Emit Test Once")) {
		if (emitterData_) {
			emitterData_->emit |= kEmitFlagEmitParticles;
		}
	}
	const char* blendModeItems[] = {
		"None",
		"Normal",
		"Add",
		"Subtract",
		"Multiply",
		"Screen"
	};
	int blendMode = static_cast<int>(config_.blendMode);
	if (ImGui::Combo(
			"BlendMode",
			&blendMode,
			blendModeItems,
			IM_ARRAYSIZE(blendModeItems)
		)) {
		config_.blendMode = static_cast<ParticleCommon::BlendMode>(blendMode);
		dirty = true;
	}

	if (ImGui::CollapsingHeader("Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
		dirty |= ImGui::DragFloat3(
			"Translate",
			&config_.emitter.translate.x,
			0.01f
		);
		dirty |= ImGui::DragFloat(
			"Radius",
			&config_.emitter.radius,
			0.01f,
			0.0f,
			100.0f
		);
		const char* shapeItems[] = { "Sphere", "Box" };
		int shape = static_cast<int>(config_.emitter.shape);
		if (ImGui::Combo("Shape", &shape, shapeItems, IM_ARRAYSIZE(shapeItems))) {
			config_.emitter.shape = static_cast<uint32_t>(std::clamp(shape, 0, 1));
			dirty = true;
		}
		dirty |= ImGui::DragFloat3(
			"Spawn Size",
			&config_.emitter.spawnSize.x,
			0.01f,
			0.0f,
			1000.0f
		);
		int count = static_cast<int>(config_.emitter.count);
		if (ImGui::DragInt(
				"Count",
				&count,
				1.0f,
				0,
				static_cast<int>(kMaxParticles)
			)) {
			config_.emitter.count =
				static_cast<uint32_t>(std::clamp(count, 0, static_cast<int>(kMaxParticles)));
			dirty = true;
		}
		dirty |= ImGui::DragFloat(
			"Frequency",
			&config_.emitter.frequency,
			0.01f,
			kMinEmitterFrequency,
			60.0f
		);
		if (ImGui::Button("Emit Once")) {
			if (emitterData_) {
				emitterData_->emit |= kEmitFlagEmitParticles;
			}
		}
	}

	if (ImGui::CollapsingHeader("Behavior", ImGuiTreeNodeFlags_DefaultOpen)) {
		BehaviorSettings& behavior = config_.behavior;
		float lifeTime[2] = { behavior.lifeTimeMin, behavior.lifeTimeMax };
		if (ImGui::DragFloat2("LifeTime", lifeTime, 0.01f, 0.01f, 60.0f)) {
			behavior.lifeTimeMin = lifeTime[0];
			behavior.lifeTimeMax = (std::max)(lifeTime[0], lifeTime[1]);
			dirty = true;
		}

		float scale[2] = { behavior.scaleMin, behavior.scaleMax };
		if (ImGui::DragFloat2("Scale", scale, 0.001f, 0.0f, 100.0f)) {
			behavior.scaleMin = scale[0];
			behavior.scaleMax = (std::max)(scale[0], scale[1]);
			behavior.startScaleMin = { behavior.scaleMin, behavior.scaleMin, 1.0f };
			behavior.startScaleMax = { behavior.scaleMax, behavior.scaleMax, 1.0f };
			dirty = true;
		}
		dirty |= ImGui::DragFloat3(
			"Start Scale Min",
			&behavior.startScaleMin.x,
			0.001f,
			0.0f,
			100.0f
		);
		dirty |= ImGui::DragFloat3(
			"Start Scale Max",
			&behavior.startScaleMax.x,
			0.001f,
			0.0f,
			100.0f
		);
		dirty |= ImGui::Checkbox("Scale Over Life", &behavior.enableScaleOverLife);
		dirty |= ImGui::DragFloat3(
			"End Scale Min",
			&behavior.endScaleMin.x,
			0.001f,
			0.0f,
			100.0f
		);
		dirty |= ImGui::DragFloat3(
			"End Scale Max",
			&behavior.endScaleMax.x,
			0.001f,
			0.0f,
			100.0f
		);

		const char* movementModeItems[] = { "Linear", "Vortex Inward" };
		int movementMode = static_cast<int>(behavior.movementMode);
		if (ImGui::Combo(
				"Movement Mode",
				&movementMode,
				movementModeItems,
				IM_ARRAYSIZE(movementModeItems)
			)) {
			behavior.movementMode =
				static_cast<uint32_t>(std::clamp(movementMode, 0, 1));
			dirty = true;
		}

		float velocity[2] = { behavior.velocityMin, behavior.velocityMax };
		if (ImGui::DragFloat2("Velocity", velocity, 0.01f, -100.0f, 100.0f)) {
			behavior.velocityMin = velocity[0];
			behavior.velocityMax = (std::max)(velocity[0], velocity[1]);
			dirty = true;
		}
		dirty |= ImGui::DragFloat3(
			"Velocity Base",
			&behavior.velocityBase.x,
			0.01f
		);
		dirty |= ImGui::DragFloat3(
			"Velocity Random Range",
			&behavior.velocityRandomRange.x,
			0.01f,
			0.0f,
			100.0f
		);
		dirty |= ImGui::DragFloat3(
			"Acceleration Base",
			&behavior.accelerationBase.x,
			0.001f
		);
		dirty |= ImGui::DragFloat3(
			"Acceleration Random Range",
			&behavior.accelerationRandomRange.x,
			0.001f,
			0.0f,
			100.0f
		);
		dirty |= ImGui::DragFloat(
			"Sway Amplitude",
			&behavior.swayAmplitude,
			0.01f,
			0.0f,
			100.0f
		);
		dirty |= ImGui::DragFloat(
			"Sway Frequency",
			&behavior.swayFrequency,
			0.01f,
			0.0f,
			100.0f
		);

		if (ImGui::TreeNode("Point Field")) {
			dirty |= ImGui::Checkbox(
				"Point Field Enabled",
				&behavior.pointFieldEnabled
			);
			dirty |= ImGui::DragFloat3(
				"Point Field Center",
				&behavior.pointFieldCenter.x,
				0.01f
			);
			dirty |= ImGui::DragFloat(
				"Point Field Radius",
				&behavior.pointFieldRadius,
				0.01f,
				0.0f,
				1000.0f
			);
			dirty |= ImGui::DragFloat(
				"Point Field Attraction",
				&behavior.pointFieldAttraction,
				0.01f,
				-1000.0f,
				1000.0f
			);
			dirty |= ImGui::DragFloat(
				"Point Field Repulsion",
				&behavior.pointFieldRepulsion,
				0.01f,
				-1000.0f,
				1000.0f
			);
			dirty |= ImGui::DragFloat(
				"Point Field Orbit",
				&behavior.pointFieldOrbit,
				0.01f,
				-1000.0f,
				1000.0f
			);
			dirty |= ImGui::DragFloat3(
				"Point Field Orbit Axis",
				&behavior.pointFieldOrbitAxis.x,
				0.01f
			);
			dirty |= ImGui::DragFloat(
				"Point Field Falloff",
				&behavior.pointFieldFalloff,
				0.01f,
				0.0f,
				100.0f
			);
			dirty |= ImGui::DragFloat(
				"Point Field Damping",
				&behavior.pointFieldDamping,
				0.01f,
				0.0f,
				100.0f
			);
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Vortex")) {
			bool rangeCollapsed = false;
			dirty |= ImGui::DragFloat3(
				"Vortex Center",
				&behavior.vortexCenter.x,
				0.01f
			);
			const char* vortexAxisItems[] = { "X", "Y", "Z" };
			int vortexAxis = static_cast<int>(behavior.vortexAxis);
			if (ImGui::Combo(
					"Vortex Axis",
					&vortexAxis,
					vortexAxisItems,
					IM_ARRAYSIZE(vortexAxisItems)
				)) {
				behavior.vortexAxis =
					static_cast<uint32_t>(std::clamp(vortexAxis, 0, 2));
				dirty = true;
			}
			float angularSpeed[2] = {
				behavior.vortexAngularSpeedMin,
				behavior.vortexAngularSpeedMax
			};
			if (ImGui::DragFloat2(
					"Vortex Angular Speed",
					angularSpeed,
					0.01f,
					-100.0f,
					100.0f
				)) {
				behavior.vortexAngularSpeedMin = angularSpeed[0];
				if (angularSpeed[1] < angularSpeed[0]) {
					angularSpeed[1] = angularSpeed[0];
					rangeCollapsed = true;
				}
				behavior.vortexAngularSpeedMax = angularSpeed[1];
				dirty = true;
			}
			float inwardSpeed[2] = {
				behavior.vortexInwardSpeedMin,
				behavior.vortexInwardSpeedMax
			};
			if (ImGui::DragFloat2(
					"Vortex Inward Speed",
					inwardSpeed,
					0.01f,
					-100.0f,
					100.0f
				)) {
				behavior.vortexInwardSpeedMin = inwardSpeed[0];
				if (inwardSpeed[1] < inwardSpeed[0]) {
					inwardSpeed[1] = inwardSpeed[0];
					rangeCollapsed = true;
				}
				behavior.vortexInwardSpeedMax = inwardSpeed[1];
				dirty = true;
			}
			float verticalSpeed[2] = {
				behavior.vortexVerticalSpeedMin,
				behavior.vortexVerticalSpeedMax
			};
			if (ImGui::DragFloat2(
					"Vortex Vertical Speed",
					verticalSpeed,
					0.01f,
					-100.0f,
					100.0f
				)) {
				behavior.vortexVerticalSpeedMin = verticalSpeed[0];
				if (verticalSpeed[1] < verticalSpeed[0]) {
					verticalSpeed[1] = verticalSpeed[0];
					rangeCollapsed = true;
				}
				behavior.vortexVerticalSpeedMax = verticalSpeed[1];
				dirty = true;
			}
			if (rangeCollapsed) {
				ImGui::TextDisabled("Max collapsed to Min for inverted vortex range.");
			}
			ImGui::TreePop();
		}

		float rotationSpeed[2] = {
			behavior.rotationSpeedMin,
			behavior.rotationSpeedMax
		};
		if (ImGui::DragFloat2(
				"Rotation Speed",
				rotationSpeed,
				0.01f,
				-100.0f,
				100.0f
			)) {
			behavior.rotationSpeedMin = rotationSpeed[0];
			behavior.rotationSpeedMax = (std::max)(rotationSpeed[0], rotationSpeed[1]);
			dirty = true;
		}
		dirty |= ImGui::Checkbox("Align To Velocity", &behavior.alignToVelocity);
		const char* alignAxisItems[] = { "X", "Y", "Z" };
		int alignAxis = static_cast<int>(behavior.alignAxis);
		if (ImGui::Combo("Align Axis", &alignAxis, alignAxisItems, IM_ARRAYSIZE(alignAxisItems))) {
			behavior.alignAxis = static_cast<uint32_t>(std::clamp(alignAxis, 0, 2));
			dirty = true;
		}

		const char* colorModeItems[] = { "Constant", "Over Life" };
		int colorMode = static_cast<int>(behavior.colorMode);
		if (ImGui::Combo("Color Mode", &colorMode, colorModeItems, IM_ARRAYSIZE(colorModeItems))) {
			behavior.colorMode = static_cast<uint32_t>(std::clamp(colorMode, 0, 1));
			dirty = true;
		}
		dirty |= ImGui::ColorEdit4("Start Color Min", &behavior.colorMin.x);
		dirty |= ImGui::ColorEdit4("Start Color Max", &behavior.colorMax.x);
		dirty |= ImGui::ColorEdit4("End Color Min", &behavior.endColorMin.x);
		dirty |= ImGui::ColorEdit4("End Color Max", &behavior.endColorMax.x);
		dirty |= ImGui::Checkbox("Life Fade", &behavior.enableLifeFade);
		dirty |= ImGui::DragFloat(
			"Fade Out Start Ratio",
			&behavior.fadeOutStartRatio,
			0.01f,
			0.0f,
			1.0f
		);
	}

	if (dirty) {
		config_.emitter.radius = (std::max)(0.0f, config_.emitter.radius);
		config_.emitter.shape = std::clamp(config_.emitter.shape, 0u, 1u);
		config_.emitter.frequency = NormalizeEmitterFrequency(config_.emitter.frequency);
		config_.behavior.fadeOutStartRatio =
			std::clamp(config_.behavior.fadeOutStartRatio, 0.0f, 1.0f);
		config_.behavior.colorMode = std::clamp(config_.behavior.colorMode, 0u, 1u);
		config_.behavior.alignAxis = std::clamp(config_.behavior.alignAxis, 0u, 2u);
		config_.behavior.swayAmplitude =
			(std::max)(0.0f, config_.behavior.swayAmplitude);
		config_.behavior.swayFrequency =
			(std::max)(0.0f, config_.behavior.swayFrequency);
		config_.behavior.pointFieldRadius =
			(std::max)(0.0f, config_.behavior.pointFieldRadius);
		config_.behavior.pointFieldFalloff =
			(std::max)(0.0f, config_.behavior.pointFieldFalloff);
		config_.behavior.pointFieldDamping =
			(std::max)(0.0f, config_.behavior.pointFieldDamping);
		config_.behavior.pointFieldOrbitAxis =
			NormalizeVectorOrZero(config_.behavior.pointFieldOrbitAxis);
		if (LengthSquared(config_.behavior.pointFieldOrbitAxis) <= 0.000001f) {
			config_.behavior.pointFieldOrbitAxis = { 0.0f, 1.0f, 0.0f };
		}
		config_.behavior.movementMode =
			std::clamp(config_.behavior.movementMode, 0u, 1u);
		config_.behavior.vortexAxis =
			std::clamp(config_.behavior.vortexAxis, 0u, 2u);
		config_.behavior.vortexAngularSpeedMax = (std::max)(
			config_.behavior.vortexAngularSpeedMin,
			config_.behavior.vortexAngularSpeedMax
		);
		config_.behavior.vortexInwardSpeedMax = (std::max)(
			config_.behavior.vortexInwardSpeedMin,
			config_.behavior.vortexInwardSpeedMax
		);
		config_.behavior.vortexVerticalSpeedMax = (std::max)(
			config_.behavior.vortexVerticalSpeedMin,
			config_.behavior.vortexVerticalSpeedMax
		);
		ApplyConfigToGpu();
		ClearParticles();
		if (emitterData_ && config_.autoEmit) {
			emitterData_->emit |= kEmitFlagEmitParticles;
		}
	}

	ImGui::End();
#else
	(void)windowTitle;
#endif
}

void GpuParticle::CreateRootSignatures() {
	HRESULT hr = S_OK;

	{
		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 1;
		ranges[0].BaseShaderRegister = 0;
		ranges[0].OffsetInDescriptorsFromTableStart =
			D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[1].NumDescriptors = 1;
		ranges[1].BaseShaderRegister = 0;
		ranges[1].OffsetInDescriptorsFromTableStart =
			D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER rootParameters[6]{};
		rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		rootParameters[0].Descriptor.ShaderRegister = 0;

		rootParameters[1].ParameterType =
			D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		rootParameters[1].DescriptorTable.pDescriptorRanges = &ranges[0];
		rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;

		rootParameters[2].ParameterType =
			D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		rootParameters[2].DescriptorTable.pDescriptorRanges = &ranges[1];
		rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;

		rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		rootParameters[3].Descriptor.ShaderRegister = 1;

		rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		rootParameters[4].Descriptor.ShaderRegister = 0;

		D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
		rootSignatureDesc.Flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
		rootSignatureDesc.pParameters = rootParameters;
		rootSignatureDesc.NumParameters = _countof(rootParameters);

		const D3D12_STATIC_SAMPLER_DESC sampler = MakeLinearSampler();
		rootSignatureDesc.pStaticSamplers = &sampler;
		rootSignatureDesc.NumStaticSamplers = 1;

		ID3DBlob* signatureBlob = nullptr;
		ID3DBlob* errorBlob = nullptr;
		hr = D3D12SerializeRootSignature(
			&rootSignatureDesc,
			D3D_ROOT_SIGNATURE_VERSION_1,
			&signatureBlob,
			&errorBlob
		);
		if (FAILED(hr)) {
			if (errorBlob) {
				Logger::Log(reinterpret_cast<char*>(errorBlob->GetBufferPointer()));
			}
			assert(false);
		}

		hr = dxCommon_->GetDevice()->CreateRootSignature(
			0,
			signatureBlob->GetBufferPointer(),
			signatureBlob->GetBufferSize(),
			IID_PPV_ARGS(&graphicsRootSignature_)
		);
		assert(SUCCEEDED(hr));
	}

	{
		D3D12_DESCRIPTOR_RANGE uavRanges[3]{};
		for (uint32_t index = 0; index < _countof(uavRanges); ++index) {
			uavRanges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			uavRanges[index].NumDescriptors = 1;
			uavRanges[index].BaseShaderRegister = index;
			uavRanges[index].OffsetInDescriptorsFromTableStart =
				D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
		}

		D3D12_ROOT_PARAMETER rootParameters[6]{};
		for (uint32_t index = 0; index < 3; ++index) {
			rootParameters[index].ParameterType =
				D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			rootParameters[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			rootParameters[index].DescriptorTable.pDescriptorRanges =
				&uavRanges[index];
			rootParameters[index].DescriptorTable.NumDescriptorRanges = 1;
		}
		rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[3].Descriptor.ShaderRegister = 0;

		rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[4].Descriptor.ShaderRegister = 2;

		rootParameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		rootParameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[5].Constants.ShaderRegister = 3;
		rootParameters[5].Constants.Num32BitValues = 4;

		D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
		rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
		rootSignatureDesc.pParameters = rootParameters;
		rootSignatureDesc.NumParameters = _countof(rootParameters);

		ID3DBlob* signatureBlob = nullptr;
		ID3DBlob* errorBlob = nullptr;
		hr = D3D12SerializeRootSignature(
			&rootSignatureDesc,
			D3D_ROOT_SIGNATURE_VERSION_1,
			&signatureBlob,
			&errorBlob
		);
		if (FAILED(hr)) {
			if (errorBlob) {
				Logger::Log(reinterpret_cast<char*>(errorBlob->GetBufferPointer()));
			}
			assert(false);
		}

		hr = dxCommon_->GetDevice()->CreateRootSignature(
			0,
			signatureBlob->GetBufferPointer(),
			signatureBlob->GetBufferSize(),
			IID_PPV_ARGS(&initializeRootSignature_)
		);
		assert(SUCCEEDED(hr));
	}

	{
		D3D12_DESCRIPTOR_RANGE uavRanges[3]{};
		for (uint32_t index = 0; index < _countof(uavRanges); ++index) {
			uavRanges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			uavRanges[index].NumDescriptors = 1;
			uavRanges[index].BaseShaderRegister = index;
			uavRanges[index].OffsetInDescriptorsFromTableStart =
				D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
		}

		D3D12_ROOT_PARAMETER rootParameters[7]{};
		rootParameters[0].ParameterType =
			D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[0].DescriptorTable.pDescriptorRanges = &uavRanges[0];
		rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;

		rootParameters[1].ParameterType =
			D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[1].DescriptorTable.pDescriptorRanges = &uavRanges[1];
		rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;

		rootParameters[2].ParameterType =
			D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[2].DescriptorTable.pDescriptorRanges = &uavRanges[2];
		rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;

		rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[3].Descriptor.ShaderRegister = 0;

		rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[4].Descriptor.ShaderRegister = 1;

		rootParameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[5].Descriptor.ShaderRegister = 2;

		rootParameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		rootParameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootParameters[6].Constants.ShaderRegister = 3;
		rootParameters[6].Constants.Num32BitValues = 4;

		D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
		rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
		rootSignatureDesc.pParameters = rootParameters;
		rootSignatureDesc.NumParameters = _countof(rootParameters);

		ID3DBlob* signatureBlob = nullptr;
		ID3DBlob* errorBlob = nullptr;
		hr = D3D12SerializeRootSignature(
			&rootSignatureDesc,
			D3D_ROOT_SIGNATURE_VERSION_1,
			&signatureBlob,
			&errorBlob
		);
		if (FAILED(hr)) {
			if (errorBlob) {
				Logger::Log(reinterpret_cast<char*>(errorBlob->GetBufferPointer()));
			}
			assert(false);
		}

		hr = dxCommon_->GetDevice()->CreateRootSignature(
			0,
			signatureBlob->GetBufferPointer(),
			signatureBlob->GetBufferSize(),
			IID_PPV_ARGS(&emitRootSignature_)
		);
		assert(SUCCEEDED(hr));
	}
}

void GpuParticle::CreatePipelineStates() {
	HRESULT hr = S_OK;

	const auto vertexShaderBlob = dxCommon_->CompileShader(
		L"resources/shaders/GpuParticle.VS.hlsl",
		L"vs_6_0"
	);
	assert(vertexShaderBlob);
	const auto pixelShaderBlob = dxCommon_->CompileShader(
		L"resources/shaders/Particle.PS.hlsl",
		L"ps_6_0"
	);
	assert(pixelShaderBlob);

	D3D12_INPUT_ELEMENT_DESC inputElementDescs[4]{};
	inputElementDescs[0].SemanticName = "POSITION";
	inputElementDescs[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	inputElementDescs[0].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElementDescs[1].SemanticName = "TEXCOORD";
	inputElementDescs[1].Format = DXGI_FORMAT_R32G32_FLOAT;
	inputElementDescs[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElementDescs[2].SemanticName = "NORMAL";
	inputElementDescs[2].Format = DXGI_FORMAT_R32G32B32_FLOAT;
	inputElementDescs[2].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
	inputElementDescs[3].SemanticName = "COLOR";
	inputElementDescs[3].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	inputElementDescs[3].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

	D3D12_INPUT_LAYOUT_DESC inputLayoutDesc{};
	inputLayoutDesc.pInputElementDescs = inputElementDescs;
	inputLayoutDesc.NumElements = _countof(inputElementDescs);

	D3D12_RASTERIZER_DESC rasterizerDesc{};
	rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
	rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rasterizerDesc.DepthClipEnable = TRUE;

	D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
	depthStencilDesc.DepthEnable = TRUE;
	depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsPipelineDesc{};
	graphicsPipelineDesc.pRootSignature = graphicsRootSignature_.Get();
	graphicsPipelineDesc.InputLayout = inputLayoutDesc;
	graphicsPipelineDesc.VS = {
		vertexShaderBlob->GetBufferPointer(),
		vertexShaderBlob->GetBufferSize()
	};
	graphicsPipelineDesc.PS = {
		pixelShaderBlob->GetBufferPointer(),
		pixelShaderBlob->GetBufferSize()
	};
	graphicsPipelineDesc.RasterizerState = rasterizerDesc;
	graphicsPipelineDesc.DepthStencilState = depthStencilDesc;
	graphicsPipelineDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	graphicsPipelineDesc.NumRenderTargets = 1;
	graphicsPipelineDesc.RTVFormats[0] = RenderFormats::kSceneHdrFormat;
	graphicsPipelineDesc.PrimitiveTopologyType =
		D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	graphicsPipelineDesc.SampleDesc.Count = 1;
	graphicsPipelineDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

	for (uint32_t blendIndex = 0;
		blendIndex <
			static_cast<uint32_t>(ParticleCommon::BlendMode::kCountOfBlendMode);
		++blendIndex) {
		D3D12_BLEND_DESC blendDesc{};
		ApplyBlendMode(
			blendDesc,
			static_cast<ParticleCommon::BlendMode>(blendIndex)
		);
		graphicsPipelineDesc.BlendState = blendDesc;
		hr = dxCommon_->GetDevice()->CreateGraphicsPipelineState(
			&graphicsPipelineDesc,
			IID_PPV_ARGS(&graphicsPipelineStates_[blendIndex])
		);
		if (LogFailedHRESULT("GpuParticle graphics PSO", hr)) {
			continue;
		}
		assert(graphicsPipelineStates_[blendIndex]);
	}

	const auto initializeShaderBlob = dxCommon_->CompileShader(
		L"resources/shaders/InitializeGpuParticle.CS.hlsl",
		L"cs_6_0"
	);
	assert(initializeShaderBlob);

	D3D12_COMPUTE_PIPELINE_STATE_DESC computePipelineDesc{};
	computePipelineDesc.pRootSignature = initializeRootSignature_.Get();
	computePipelineDesc.CS = {
		initializeShaderBlob->GetBufferPointer(),
		initializeShaderBlob->GetBufferSize()
	};

	hr = dxCommon_->GetDevice()->CreateComputePipelineState(
		&computePipelineDesc,
		IID_PPV_ARGS(&initializePipelineState_)
	);
	if (!LogFailedHRESULT("GpuParticle initialize PSO", hr)) {
		assert(initializePipelineState_);
	}

	const auto emitShaderBlob = dxCommon_->CompileShader(
		L"resources/shaders/EmitGpuParticle.CS.hlsl",
		L"cs_6_0"
	);
	assert(emitShaderBlob);

	D3D12_COMPUTE_PIPELINE_STATE_DESC emitPipelineDesc{};
	emitPipelineDesc.pRootSignature = emitRootSignature_.Get();
	emitPipelineDesc.CS = {
		emitShaderBlob->GetBufferPointer(),
		emitShaderBlob->GetBufferSize()
	};

	hr = dxCommon_->GetDevice()->CreateComputePipelineState(
		&emitPipelineDesc,
		IID_PPV_ARGS(&emitPipelineState_)
	);
	if (!LogFailedHRESULT("GpuParticle emit PSO", hr)) {
		assert(emitPipelineState_);
	}

	const auto updateShaderBlob = dxCommon_->CompileShader(
		L"resources/shaders/UpdateGpuParticle.CS.hlsl",
		L"cs_6_0"
	);
	assert(updateShaderBlob);

	D3D12_COMPUTE_PIPELINE_STATE_DESC updatePipelineDesc{};
	updatePipelineDesc.pRootSignature = emitRootSignature_.Get();
	updatePipelineDesc.CS = {
		updateShaderBlob->GetBufferPointer(),
		updateShaderBlob->GetBufferSize()
	};

	hr = dxCommon_->GetDevice()->CreateComputePipelineState(
		&updatePipelineDesc,
		IID_PPV_ARGS(&updatePipelineState_)
	);
	if (!LogFailedHRESULT("GpuParticle update PSO", hr)) {
		assert(updatePipelineState_);
	}
}

void GpuParticle::TransitionParticleResource(D3D12_RESOURCE_STATES stateAfter) {
	if (!particleResource_ || particleResourceState_ == stateAfter) {
		return;
	}

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = particleResource_.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = particleResourceState_;
	barrier.Transition.StateAfter = stateAfter;
	dxCommon_->GetCommandList()->ResourceBarrier(1, &barrier);
	particleResourceState_ = stateAfter;
}

void GpuParticle::TransitionFreeListIndexResource(
	D3D12_RESOURCE_STATES stateAfter
) {
	if (!freeListIndexResource_ || freeListIndexResourceState_ == stateAfter) {
		return;
	}

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = freeListIndexResource_.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = freeListIndexResourceState_;
	barrier.Transition.StateAfter = stateAfter;
	dxCommon_->GetCommandList()->ResourceBarrier(1, &barrier);
	freeListIndexResourceState_ = stateAfter;
}

void GpuParticle::TransitionFreeListResource(D3D12_RESOURCE_STATES stateAfter) {
	if (!freeListResource_ || freeListResourceState_ == stateAfter) {
		return;
	}

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = freeListResource_.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = freeListResourceState_;
	barrier.Transition.StateAfter = stateAfter;
	dxCommon_->GetCommandList()->ResourceBarrier(1, &barrier);
	freeListResourceState_ = stateAfter;
}

void GpuParticle::InitializeParticlesOnGPU() {
	if (!needsInitialize_) {
		return;
	}
	if (
		!dxCommon_ ||
		!srvManager_ ||
		!particleResource_ ||
		!freeListIndexResource_ ||
		!freeListResource_ ||
		!emitterResource_ ||
		!behaviorResource_ ||
		!initializeRootSignature_ ||
		!initializePipelineState_
	) {
		static bool loggedInitializeUnavailable = false;
		if (!loggedInitializeUnavailable) {
			Logger::Log(
				"GpuParticle initialize skipped: required compute resources are not ready\n"
			);
			loggedInitializeUnavailable = true;
		}
		return;
	}

	auto* commandList = dxCommon_->GetCommandList();
	srvManager_->PreDraw();
	TransitionParticleResource(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	TransitionFreeListIndexResource(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	TransitionFreeListResource(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	commandList->SetComputeRootSignature(initializeRootSignature_.Get());
	commandList->SetPipelineState(initializePipelineState_.Get());
	const bool descriptorsBound =
		srvManager_->SetComputeRootDescriptorTable(0, particleUavIndex_) &&
		srvManager_->SetComputeRootDescriptorTable(1, freeListIndexUavIndex_) &&
		srvManager_->SetComputeRootDescriptorTable(2, freeListUavIndex_);
	if (!descriptorsBound) {
		return;
	}
	commandList->SetComputeRootConstantBufferView(
		3,
		emitterResource_->GetGPUVirtualAddress()
	);
	commandList->SetComputeRootConstantBufferView(
		4,
		behaviorResource_->GetGPUVirtualAddress()
	);
	const uint32_t dispatchConstants[4] = {
		emitterData_ ? emitterData_->emit : 0u,
		0u,
		0u,
		0u
	};
	commandList->SetComputeRoot32BitConstants(
		5,
		static_cast<UINT>(_countof(dispatchConstants)),
		dispatchConstants,
		0
	);
	commandList->Dispatch(GpuParticleDispatchGroupCount(), 1, 1);

	D3D12_RESOURCE_BARRIER uavBarriers[3]{};
	uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarriers[0].UAV.pResource = particleResource_.Get();
	uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarriers[1].UAV.pResource = freeListIndexResource_.Get();
	uavBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarriers[2].UAV.pResource = freeListResource_.Get();
	commandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);

	TransitionParticleResource(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	if (emitterData_) {
		emitterData_->emit &= ~kEmitFlagSeedVisibleParticle;
	}
	needsInitialize_ = false;
}

void GpuParticle::EmitParticlesOnGPU() {
	if (!emitterData_ || (emitterData_->emit & kEmitFlagEmitParticles) == 0) {
		return;
	}
	if (
		!dxCommon_ ||
		!srvManager_ ||
		!particleResource_ ||
		!freeListIndexResource_ ||
		!freeListResource_ ||
		!emitterResource_ ||
		!perFrameResource_ ||
		!behaviorResource_ ||
		!emitRootSignature_ ||
		!emitPipelineState_
	) {
		static bool loggedEmitUnavailable = false;
		if (!loggedEmitUnavailable) {
			Logger::Log(
				"GpuParticle emit skipped: required compute resources are not ready\n"
			);
			loggedEmitUnavailable = true;
		}
		return;
	}

	auto* commandList = dxCommon_->GetCommandList();
	srvManager_->PreDraw();
	TransitionParticleResource(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	TransitionFreeListIndexResource(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	TransitionFreeListResource(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	commandList->SetComputeRootSignature(emitRootSignature_.Get());
	commandList->SetPipelineState(emitPipelineState_.Get());
	const bool descriptorsBound =
		srvManager_->SetComputeRootDescriptorTable(0, particleUavIndex_) &&
		srvManager_->SetComputeRootDescriptorTable(1, freeListIndexUavIndex_) &&
		srvManager_->SetComputeRootDescriptorTable(2, freeListUavIndex_);
	if (!descriptorsBound) {
		return;
	}
	commandList->SetComputeRootConstantBufferView(
		3,
		emitterResource_->GetGPUVirtualAddress()
	);
	commandList->SetComputeRootConstantBufferView(
		4,
		perFrameResource_->GetGPUVirtualAddress()
	);
	commandList->SetComputeRootConstantBufferView(
		5,
		behaviorResource_->GetGPUVirtualAddress()
	);
	const uint32_t dispatchConstants[4] = {
		emitterData_->emit,
		0u,
		0u,
		0u
	};
	commandList->SetComputeRoot32BitConstants(
		6,
		static_cast<UINT>(_countof(dispatchConstants)),
		dispatchConstants,
		0
	);
	commandList->Dispatch(1, 1, 1);

	D3D12_RESOURCE_BARRIER uavBarriers[3]{};
	uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarriers[0].UAV.pResource = particleResource_.Get();
	uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarriers[1].UAV.pResource = freeListIndexResource_.Get();
	uavBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarriers[2].UAV.pResource = freeListResource_.Get();
	commandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);

	emitterData_->emit &= ~kEmitFlagEmitParticles;
}

void GpuParticle::UpdateParticlesOnGPU() {
	if (
		!dxCommon_ ||
		!srvManager_ ||
		!particleResource_ ||
		!freeListIndexResource_ ||
		!freeListResource_ ||
		!emitterResource_ ||
		!perFrameResource_ ||
		!behaviorResource_ ||
		!emitRootSignature_ ||
		!updatePipelineState_
	) {
		static bool loggedUpdateUnavailable = false;
		if (!loggedUpdateUnavailable) {
			Logger::Log(
				"GpuParticle update skipped: required compute resources are not ready\n"
			);
			loggedUpdateUnavailable = true;
		}
		return;
	}
	auto* commandList = dxCommon_->GetCommandList();
	srvManager_->PreDraw();
	TransitionParticleResource(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	TransitionFreeListIndexResource(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	TransitionFreeListResource(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	commandList->SetComputeRootSignature(emitRootSignature_.Get());
	commandList->SetPipelineState(updatePipelineState_.Get());
	const bool descriptorsBound =
		srvManager_->SetComputeRootDescriptorTable(0, particleUavIndex_) &&
		srvManager_->SetComputeRootDescriptorTable(1, freeListIndexUavIndex_) &&
		srvManager_->SetComputeRootDescriptorTable(2, freeListUavIndex_);
	if (!descriptorsBound) {
		return;
	}
	commandList->SetComputeRootConstantBufferView(
		3,
		emitterResource_->GetGPUVirtualAddress()
	);
	commandList->SetComputeRootConstantBufferView(
		4,
		perFrameResource_->GetGPUVirtualAddress()
	);
	commandList->SetComputeRootConstantBufferView(
		5,
		behaviorResource_->GetGPUVirtualAddress()
	);
	const uint32_t dispatchConstants[4] = {};
	commandList->SetComputeRoot32BitConstants(
		6,
		static_cast<UINT>(_countof(dispatchConstants)),
		dispatchConstants,
		0
	);
	commandList->Dispatch(GpuParticleDispatchGroupCount(), 1, 1);

	D3D12_RESOURCE_BARRIER uavBarriers[3]{};
	uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarriers[0].UAV.pResource = particleResource_.Get();
	uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarriers[1].UAV.pResource = freeListIndexResource_.Get();
	uavBarriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBarriers[2].UAV.pResource = freeListResource_.Get();
	commandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);

	TransitionParticleResource(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void GpuParticle::Update() {
	if (!IsInitialized() || !emitterData_ || !perFrameData_) {
		return;
	}

	elapsedTime_ += deltaTime_;
	perFrameData_->time = elapsedTime_;
	perFrameData_->deltaTime = deltaTime_;

	if (!config_.autoEmit) {
		return;
	}

	const bool emitRequested =
		(emitterData_->emit & kEmitFlagEmitParticles) != 0;
	emitterData_->frequency = NormalizeEmitterFrequency(emitterData_->frequency);
	emitterData_->frequencyTime += deltaTime_;
	if (emitterData_->frequency <= emitterData_->frequencyTime) {
		emitterData_->frequencyTime -= emitterData_->frequency;
		emitterData_->emit |= kEmitFlagEmitParticles;
	}
	else if (!emitRequested) {
		emitterData_->emit &= ~kEmitFlagEmitParticles;
	}
}

void GpuParticle::Draw(Camera* camera) {
	if (!camera || !IsInitialized()) {
		return;
	}
	if (
		!dxCommon_ ||
		!srvManager_ ||
		!particleCommon_ ||
		!graphicsRootSignature_ ||
		!materialResource_ ||
		!directionalLightResource_ ||
		!perViewResource_
	) {
		static bool loggedDrawUnavailable = false;
		if (!loggedDrawUnavailable) {
			Logger::Log(
				"GpuParticle draw skipped: required graphics resources are not ready\n"
			);
			loggedDrawUnavailable = true;
		}
		return;
	}
	static bool loggedFirstDraw = false;
	if (!loggedFirstDraw) {
		Logger::Log("GpuParticle::Draw reached\n");
		loggedFirstDraw = true;
	}

	InitializeParticlesOnGPU();
	if (needsInitialize_) {
		return;
	}
	EmitParticlesOnGPU();
	UpdateParticlesOnGPU();
	TransitionParticleResource(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	perViewData_->viewProjection = camera->GetViewProjectionMatrix();
	perViewData_->billboardMatrix = camera->GetWorldMatrix();
	perViewData_->billboardMatrix.m[3][0] = 0.0f;
	perViewData_->billboardMatrix.m[3][1] = 0.0f;
	perViewData_->billboardMatrix.m[3][2] = 0.0f;
	perViewData_->renderFlags = {
		config_.useBillboard ? 1.0f : 0.0f,
		config_.forceVisible ? 1.0f : 0.0f,
		0.0f,
		0.0f
	};

	auto* commandList = dxCommon_->GetCommandList();
	commandList->SetGraphicsRootSignature(graphicsRootSignature_.Get());
	const uint32_t blendIndex = std::clamp(
		static_cast<uint32_t>(config_.blendMode),
		0u,
		static_cast<uint32_t>(ParticleCommon::BlendMode::kCountOfBlendMode) - 1u
	);
	if (!graphicsPipelineStates_[blendIndex]) {
		static bool loggedGraphicsPsoUnavailable = false;
		if (!loggedGraphicsPsoUnavailable) {
			Logger::Log("GpuParticle draw skipped: graphics PSO is not ready\n");
			loggedGraphicsPsoUnavailable = true;
		}
		return;
	}
	srvManager_->PreDraw();
	commandList->SetPipelineState(graphicsPipelineStates_[blendIndex].Get());
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	const D3D12_VERTEX_BUFFER_VIEW& vbv =
		particleCommon_->GetVertexBufferView();
	commandList->IASetVertexBuffers(0, 1, &vbv);

	commandList->SetGraphicsRootConstantBufferView(
		0,
		materialResource_->GetGPUVirtualAddress()
	);
	const bool descriptorsBound =
		srvManager_->SetGraphicsRootDescriptorTable(1, particleSrvIndex_) &&
		srvManager_->SetGraphicsRootDescriptorTable(2, textureSrvIndex_);
	if (!descriptorsBound) {
		return;
	}
	commandList->SetGraphicsRootConstantBufferView(
		3,
		directionalLightResource_->GetGPUVirtualAddress()
	);
	commandList->SetGraphicsRootConstantBufferView(
		4,
		perViewResource_->GetGPUVirtualAddress()
	);

	commandList->DrawInstanced(6, kMaxParticles, 0, 0);
}
