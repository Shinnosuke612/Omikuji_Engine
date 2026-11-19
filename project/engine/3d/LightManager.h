// 役割: Directional、Point、Spot Lightの定数Bufferを管理する。
#pragma once
#include "../scene/SceneRuntimePreparation.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <wrl.h>
#include <d3d12.h>

#include "../math/Vector3.h"
#include "../math/Vector4.h"

class DirectXCommon;

class LightManager {
public:
	static const uint32_t kMaxPointLights = 16;
	static const uint32_t kMaxSpotLights = 8;

	struct DirectionalLight {
		Vector4 color;
		Vector3 direction;
		float intensity;
		int32_t enable;
		float padding[3];
	};

	struct PointLight {
		Vector4 color;
		Vector3 position;
		float intensity;
		float radius;
		float decay;
		int32_t enable;
		float padding;
	};

	struct SpotLight {
		Vector4 color;
		Vector3 position;
		float intensity;
		Vector3 direction;
		float distance;
		float decay;
		float cosAngle;
		float cosFalloffStart;
		int32_t enable;
	};

	struct LightingForGPU {
		DirectionalLight directionalLight;

		int32_t pointLightCount;
		int32_t spotLightCount;
		float padding[2];

		PointLight pointLights[kMaxPointLights];
		SpotLight spotLights[kMaxSpotLights];
	};

	struct ShadowSettings {
		int32_t enable = false;
		float bias = 0.0025f;
		float normalBias = 0.02f;
		float strength = 0.55f;
		Vector3 target = { 0.0f, 0.0f, 0.0f };
		float distance = 45.0f;
		float orthographicSize = 40.0f;
		float nearClip = 0.1f;
		float farClip = 120.0f;
		bool texelSnap = true;
	};

public:
	void Initialize(DirectXCommon* dxCommon, const std::string& jsonPath = "");
	// main専用。新規候補の既定値を設定し、SceneがDocumentの照明値を反映する。
	bool BeginPreparedInitialize(DirectXCommon&, std::string& diagnostic);
	RuntimePrepareStep StepPreparedInitialize(SceneRuntimePrepareBudget&, std::string& diagnostic);
    // 新規instance専用。Scene/JSONを読み込まず固定したlight値をコピーする。
    bool TryInitializeReplay(DirectXCommon* dx, const LightManager& source);
	void Reset();

	void Bind(ID3D12GraphicsCommandList* commandList, UINT rootParameterIndex);
	void DrawImGui();

	void SetJsonPath(const std::string& jsonPath) { jsonPath_ = jsonPath; }

	bool LoadFromJson(const std::string& jsonPath);
	bool SaveToJson(const std::string& jsonPath) const;

	void AddPointLight(const PointLight& light);
	void AddSpotLight(const SpotLight& light);

	void RemovePointLight(size_t index);
	void RemoveSpotLight(size_t index);

	void ClearPointLights();
	void ClearSpotLights();

	void SyncToGPU();

	void SetShadowMapSize(uint32_t shadowMapSize);
	uint32_t GetShadowMapSize() const { return shadowMapSize_; }

	DirectionalLight& GetDirectionalLight() { return directionalLight_; }
	std::vector<PointLight>& GetPointLights() { return pointLights_; }
	std::vector<SpotLight>& GetSpotLights() { return spotLights_; }
	ShadowSettings& GetDirectionalShadowSettings() { return directionalShadowSettings_; }
	std::vector<ShadowSettings>& GetPointShadowSettings() { return pointShadowSettings_; }
	std::vector<ShadowSettings>& GetSpotShadowSettings() { return spotShadowSettings_; }

	const DirectionalLight& GetDirectionalLight() const { return directionalLight_; }
	const std::vector<PointLight>& GetPointLights() const { return pointLights_; }
	const std::vector<SpotLight>& GetSpotLights() const { return spotLights_; }
	const ShadowSettings& GetDirectionalShadowSettings() const { return directionalShadowSettings_; }
	const std::vector<ShadowSettings>& GetPointShadowSettings() const { return pointShadowSettings_; }
	const std::vector<ShadowSettings>& GetSpotShadowSettings() const { return spotShadowSettings_; }

private:
	PointLight MakeDefaultPointLight() const;
	SpotLight MakeDefaultSpotLight() const;
	ShadowSettings MakeDefaultShadowSettings(bool enable) const;
	bool DrawShadowSettingsImGui(
		const char* label,
		ShadowSettings& settings,
		bool canRender,
		bool showDirectionalCameraSettings = false
	);

private:
	DirectXCommon* dxCommon_ = nullptr;

	Microsoft::WRL::ComPtr<ID3D12Resource> lightingResource_;
	LightingForGPU* lightingData_ = nullptr;
	RuntimePrepareStep preparedState_ = RuntimePrepareStep::Failed;

	std::string jsonPath_;

	DirectionalLight directionalLight_{};
	std::vector<PointLight> pointLights_;
	std::vector<SpotLight> spotLights_;
	ShadowSettings directionalShadowSettings_{};
	std::vector<ShadowSettings> pointShadowSettings_;
	std::vector<ShadowSettings> spotShadowSettings_;
	uint32_t shadowMapSize_ = 2048;
};
