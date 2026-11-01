// 役割: ライト設定の初期化とGPU定数Buffer更新を実装する。
#include "LightManager.h"

#include "../base/DirectXCommon.h"
#include "../externals/imgui/imgui.h"
#include "../externals/nlohmann/json.hpp"
#include "../utility/EditableResourcePath.h"

#include "../math/math.h"
#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

namespace {
	constexpr uint32_t kDefaultShadowMapSize = 2048;
	constexpr uint32_t kShadowMapOptions[] = {
		1024,
		2048,
		4096
	};

	json ToJson(const Vector3& v) {
		return json::array({ v.x, v.y, v.z });
	}

	json ToJson(const Vector4& v) {
		return json::array({ v.x, v.y, v.z, v.w });
	}

	Vector3 ToVector3(const json& j, const Vector3& defaultValue) {
		if (!j.is_array() || j.size() < 3) {
			return defaultValue;
		}

		return {
			j[0].get<float>(),
			j[1].get<float>(),
			j[2].get<float>()
		};
	}

	Vector4 ToVector4(const json& j, const Vector4& defaultValue) {
		if (!j.is_array() || j.size() < 4) {
			return defaultValue;
		}

		return {
			j[0].get<float>(),
			j[1].get<float>(),
			j[2].get<float>(),
			j[3].get<float>()
		};
	}

	uint32_t NormalizeShadowMapSize(uint32_t value) {
		uint32_t best = kDefaultShadowMapSize;
		uint32_t bestDistance = UINT32_MAX;
		for (uint32_t option : kShadowMapOptions) {
			const uint32_t distance = value > option
				? value - option
				: option - value;
			if (distance < bestDistance) {
				best = option;
				bestDistance = distance;
			}
		}
		return best;
	}

	int ShadowMapSizeOptionIndex(uint32_t value) {
		const uint32_t normalized = NormalizeShadowMapSize(value);
		for (uint32_t index = 0; index < _countof(kShadowMapOptions); ++index) {
			if (kShadowMapOptions[index] == normalized) {
				return static_cast<int>(index);
			}
		}
		return 1;
	}
}

void LightManager::Initialize(DirectXCommon* dxCommon, const std::string& jsonPath) {
	assert(dxCommon);

	dxCommon_ = dxCommon;
	jsonPath_ = jsonPath;

	lightingResource_ = dxCommon_->CreateBufferResource(sizeof(LightingForGPU));
	lightingResource_->Map(0, nullptr, reinterpret_cast<void**>(&lightingData_));

	Reset();

	if (!jsonPath_.empty()) {
		LoadFromJson(jsonPath_);
	}

	SyncToGPU();
}

bool LightManager::TryInitializeReplay(DirectXCommon* dx, const LightManager& source) {
    if (!dx || dxCommon_) return false;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource; LightingForGPU* data = nullptr;
    if (!dx->TryCreateBufferResource(sizeof(LightingForGPU), resource) ||
        FAILED(resource->Map(0, nullptr, reinterpret_cast<void**>(&data))) || !data) return false;
    directionalLight_ = source.directionalLight_; directionalShadowSettings_ = source.directionalShadowSettings_;
    pointLights_ = source.pointLights_; spotLights_ = source.spotLights_;
    pointShadowSettings_ = source.pointShadowSettings_; spotShadowSettings_ = source.spotShadowSettings_;
    shadowMapSize_ = source.shadowMapSize_;
    lightingResource_ = std::move(resource); lightingData_ = data; dxCommon_ = dx;
    SyncToGPU(); return true;
}

void LightManager::Reset() {
	directionalLight_.color = { 1.0f, 1.0f, 1.0f, 1.0f };
	directionalLight_.direction = { 0.0f, -1.0f, 0.0f };
	directionalLight_.intensity = 1.0f;
	directionalLight_.enable = true;
	directionalLight_.padding[0] = 0.0f;
	directionalLight_.padding[1] = 0.0f;
	directionalLight_.padding[2] = 0.0f;
	directionalShadowSettings_ = MakeDefaultShadowSettings(true);
	shadowMapSize_ = kDefaultShadowMapSize;

	pointLights_.clear();
	spotLights_.clear();
	pointShadowSettings_.clear();
	spotShadowSettings_.clear();

	SyncToGPU();
}

void LightManager::Bind(ID3D12GraphicsCommandList* commandList, UINT rootParameterIndex) {
	assert(commandList);
	assert(lightingResource_);

	commandList->SetGraphicsRootConstantBufferView(
		rootParameterIndex,
		lightingResource_->GetGPUVirtualAddress()
	);
}

void LightManager::SyncToGPU() {
	if (!lightingData_) {
		return;
	}

	*lightingData_ = {};

	lightingData_->directionalLight = directionalLight_;

	const uint32_t pointCount = std::min<uint32_t>(
		static_cast<uint32_t>(pointLights_.size()),
		kMaxPointLights
	);

	const uint32_t spotCount = std::min<uint32_t>(
		static_cast<uint32_t>(spotLights_.size()),
		kMaxSpotLights
	);

	lightingData_->pointLightCount = static_cast<int32_t>(pointCount);
	lightingData_->spotLightCount = static_cast<int32_t>(spotCount);

	for (uint32_t i = 0; i < pointCount; ++i) {
		lightingData_->pointLights[i] = pointLights_[i];
	}

	for (uint32_t i = 0; i < spotCount; ++i) {
		lightingData_->spotLights[i] = spotLights_[i];

		if (Math::Length(lightingData_->spotLights[i].direction) > 0.000001f) {
			lightingData_->spotLights[i].direction = Math::Normalize(lightingData_->spotLights[i].direction);
		}
		else {
			lightingData_->spotLights[i].direction = { 0.0f, -1.0f, 0.0f };
		}
	}
}

LightManager::PointLight LightManager::MakeDefaultPointLight() const {
	PointLight light{};
	light.color = { 1.0f, 0.85f, 0.65f, 1.0f };
	light.position = { 0.0f, 3.0f, -3.0f };
	light.intensity = 2.0f;
	light.radius = 8.0f;
	light.decay = 1.0f;
	light.enable = true;
	light.padding = 0.0f;
	return light;
}

LightManager::SpotLight LightManager::MakeDefaultSpotLight() const {
	SpotLight light{};
	light.color = { 0.75f, 0.85f, 1.0f, 1.0f };
	light.position = { 0.0f, 5.0f, -6.0f };
	light.intensity = 4.0f;
	light.direction = { 0.0f, -0.65f, 0.76f };
	light.distance = 15.0f;
	light.decay = 1.0f;
	light.cosAngle = 0.70710678f;
	light.cosFalloffStart = 0.86602540f;
	light.enable = true;
	return light;
}

LightManager::ShadowSettings LightManager::MakeDefaultShadowSettings(bool enable) const {
	ShadowSettings settings{};
	settings.enable = enable;
	settings.bias = 0.0025f;
	settings.normalBias = 0.02f;
	settings.strength = 0.55f;
	settings.target = { 0.0f, 0.0f, 0.0f };
	settings.distance = 45.0f;
	settings.orthographicSize = 40.0f;
	settings.nearClip = 0.1f;
	settings.farClip = 120.0f;
	settings.texelSnap = true;
	return settings;
}

void LightManager::AddPointLight(const PointLight& light) {
	if (pointLights_.size() >= kMaxPointLights) {
		return;
	}

	pointLights_.push_back(light);
	pointShadowSettings_.push_back(MakeDefaultShadowSettings(false));
	SyncToGPU();
}

void LightManager::AddSpotLight(const SpotLight& light) {
	if (spotLights_.size() >= kMaxSpotLights) {
		return;
	}

	spotLights_.push_back(light);
	spotShadowSettings_.push_back(MakeDefaultShadowSettings(false));
	SyncToGPU();
}

void LightManager::RemovePointLight(size_t index) {
	if (index >= pointLights_.size()) {
		return;
	}

	pointLights_.erase(pointLights_.begin() + index);
	if (index < pointShadowSettings_.size()) {
		pointShadowSettings_.erase(pointShadowSettings_.begin() + index);
	}
	SyncToGPU();
}

void LightManager::RemoveSpotLight(size_t index) {
	if (index >= spotLights_.size()) {
		return;
	}

	spotLights_.erase(spotLights_.begin() + index);
	if (index < spotShadowSettings_.size()) {
		spotShadowSettings_.erase(spotShadowSettings_.begin() + index);
	}
	SyncToGPU();
}

void LightManager::ClearPointLights() {
	pointLights_.clear();
	pointShadowSettings_.clear();
	SyncToGPU();
}

void LightManager::ClearSpotLights() {
	spotLights_.clear();
	spotShadowSettings_.clear();
	SyncToGPU();
}

bool LightManager::LoadFromJson(const std::string& jsonPath) {
	json root;
	const std::filesystem::path resolvedPath =
		EditableResourcePath::Resolve(jsonPath);
	const std::filesystem::path candidates[] = {
		resolvedPath,
		EditableResourcePath::BackupPath(resolvedPath)
	};
	bool parsed = false;
	for (const std::filesystem::path& candidate : candidates) {
		std::string text;
		if (!EditableResourcePath::ReadTextFile(candidate, text)) {
			continue;
		}
		try {
			root = json::parse(text);
			parsed = true;
			break;
		}
		catch (...) {
		}
	}
	if (!parsed) {
		return false;
	}

	if (root.contains("directionalLight")) {
		const json& j = root["directionalLight"];

		directionalLight_.color = ToVector4(j.value("color", json::array()), directionalLight_.color);
		directionalLight_.direction = ToVector3(j.value("direction", json::array()), directionalLight_.direction);
		directionalLight_.intensity = j.value("intensity", directionalLight_.intensity);
		directionalLight_.enable = j.value("enable", directionalLight_.enable);

		if (j.contains("shadow")) {
			const json& shadow = j["shadow"];
			directionalShadowSettings_.enable = shadow.value("enable", directionalShadowSettings_.enable);
			directionalShadowSettings_.bias = shadow.value("bias", directionalShadowSettings_.bias);
			directionalShadowSettings_.normalBias = shadow.value("normalBias", directionalShadowSettings_.normalBias);
			directionalShadowSettings_.strength = shadow.value("strength", directionalShadowSettings_.strength);
			directionalShadowSettings_.target = ToVector3(shadow.value("target", json::array()), directionalShadowSettings_.target);
			directionalShadowSettings_.distance = shadow.value("distance", directionalShadowSettings_.distance);
			directionalShadowSettings_.orthographicSize = shadow.value("orthographicSize", directionalShadowSettings_.orthographicSize);
			directionalShadowSettings_.nearClip = shadow.value("nearClip", directionalShadowSettings_.nearClip);
			directionalShadowSettings_.farClip = shadow.value("farClip", directionalShadowSettings_.farClip);
			directionalShadowSettings_.texelSnap = shadow.value("texelSnap", directionalShadowSettings_.texelSnap);
		}
	}

	if (root.contains("shadowQuality")) {
		const json& shadowQuality = root["shadowQuality"];
		shadowMapSize_ = NormalizeShadowMapSize(
			shadowQuality.value("mapSize", shadowMapSize_)
		);
	} else if (root.contains("shadowMapSize")) {
		shadowMapSize_ = NormalizeShadowMapSize(
			root.value("shadowMapSize", shadowMapSize_)
		);
	}

	pointLights_.clear();
	pointShadowSettings_.clear();
	if (root.contains("pointLights") && root["pointLights"].is_array()) {
		for (const json& j : root["pointLights"]) {
			if (pointLights_.size() >= kMaxPointLights) {
				break;
			}

			PointLight light = MakeDefaultPointLight();
			light.color = ToVector4(j.value("color", json::array()), light.color);
			light.position = ToVector3(j.value("position", json::array()), light.position);
			light.intensity = j.value("intensity", light.intensity);
			light.radius = j.value("radius", light.radius);
			light.decay = j.value("decay", light.decay);
			light.enable = j.value("enable", light.enable);
			light.padding = 0.0f;

			pointLights_.push_back(light);

			ShadowSettings shadow = MakeDefaultShadowSettings(false);
			if (j.contains("shadow")) {
				const json& shadowJson = j["shadow"];
				shadow.enable = shadowJson.value("enable", shadow.enable);
				shadow.bias = shadowJson.value("bias", shadow.bias);
				shadow.normalBias = shadowJson.value("normalBias", shadow.normalBias);
				shadow.strength = shadowJson.value("strength", shadow.strength);
			}
			pointShadowSettings_.push_back(shadow);
		}
	}

	spotLights_.clear();
	spotShadowSettings_.clear();
	if (root.contains("spotLights") && root["spotLights"].is_array()) {
		for (const json& j : root["spotLights"]) {
			if (spotLights_.size() >= kMaxSpotLights) {
				break;
			}

			SpotLight light = MakeDefaultSpotLight();
			light.color = ToVector4(j.value("color", json::array()), light.color);
			light.position = ToVector3(j.value("position", json::array()), light.position);
			light.intensity = j.value("intensity", light.intensity);
			light.direction = ToVector3(j.value("direction", json::array()), light.direction);
			light.distance = j.value("distance", light.distance);
			light.decay = j.value("decay", light.decay);
			light.cosAngle = j.value("cosAngle", light.cosAngle);
			light.cosFalloffStart = j.value("cosFalloffStart", light.cosFalloffStart);
			light.enable = j.value("enable", light.enable);

			spotLights_.push_back(light);

			ShadowSettings shadow = MakeDefaultShadowSettings(false);
			if (j.contains("shadow")) {
				const json& shadowJson = j["shadow"];
				shadow.enable = shadowJson.value("enable", shadow.enable);
				shadow.bias = shadowJson.value("bias", shadow.bias);
				shadow.normalBias = shadowJson.value("normalBias", shadow.normalBias);
				shadow.strength = shadowJson.value("strength", shadow.strength);
			}
			spotShadowSettings_.push_back(shadow);
		}
	}

	jsonPath_ = jsonPath;
	SyncToGPU();
	return true;
}

bool LightManager::SaveToJson(const std::string& jsonPath) const {
	json root;

	root["shadowQuality"] = {
		{ "mapSize", shadowMapSize_ }
	};

	root["directionalLight"] = {
		{ "color", ToJson(directionalLight_.color) },
		{ "direction", ToJson(directionalLight_.direction) },
		{ "intensity", directionalLight_.intensity },
		{ "enable", directionalLight_.enable },
			{ "shadow", {
			{ "enable", directionalShadowSettings_.enable },
			{ "bias", directionalShadowSettings_.bias },
			{ "normalBias", directionalShadowSettings_.normalBias },
			{ "strength", directionalShadowSettings_.strength },
			{ "target", ToJson(directionalShadowSettings_.target) },
			{ "distance", directionalShadowSettings_.distance },
			{ "orthographicSize", directionalShadowSettings_.orthographicSize },
			{ "nearClip", directionalShadowSettings_.nearClip },
			{ "farClip", directionalShadowSettings_.farClip },
			{ "texelSnap", directionalShadowSettings_.texelSnap }
		} }
	};

	root["pointLights"] = json::array();
	for (size_t i = 0; i < pointLights_.size(); ++i) {
		const PointLight& light = pointLights_[i];
		const ShadowSettings shadow =
			i < pointShadowSettings_.size() ? pointShadowSettings_[i] : MakeDefaultShadowSettings(false);
		root["pointLights"].push_back({
			{ "color", ToJson(light.color) },
			{ "position", ToJson(light.position) },
			{ "intensity", light.intensity },
			{ "radius", light.radius },
			{ "decay", light.decay },
			{ "enable", light.enable },
			{ "shadow", {
				{ "enable", shadow.enable },
				{ "bias", shadow.bias },
				{ "normalBias", shadow.normalBias },
				{ "strength", shadow.strength }
			} }
			});
	}

	root["spotLights"] = json::array();
	for (size_t i = 0; i < spotLights_.size(); ++i) {
		const SpotLight& light = spotLights_[i];
		const ShadowSettings shadow =
			i < spotShadowSettings_.size() ? spotShadowSettings_[i] : MakeDefaultShadowSettings(false);
		root["spotLights"].push_back({
			{ "color", ToJson(light.color) },
			{ "position", ToJson(light.position) },
			{ "intensity", light.intensity },
			{ "direction", ToJson(light.direction) },
			{ "distance", light.distance },
			{ "decay", light.decay },
			{ "cosAngle", light.cosAngle },
			{ "cosFalloffStart", light.cosFalloffStart },
			{ "enable", light.enable },
			{ "shadow", {
				{ "enable", shadow.enable },
				{ "bias", shadow.bias },
				{ "normalBias", shadow.normalBias },
				{ "strength", shadow.strength }
			} }
			});
	}

	std::ostringstream output;
	output << std::setw(4) << root << '\n';
	return EditableResourcePath::WriteTextAtomically(jsonPath, output.str());
}

void LightManager::SetShadowMapSize(uint32_t shadowMapSize) {
	shadowMapSize_ = NormalizeShadowMapSize(shadowMapSize);
}

bool LightManager::DrawShadowSettingsImGui(
	const char* label,
	ShadowSettings& settings,
	bool canRender,
	bool showDirectionalCameraSettings
) {
	bool changed = false;
	if (ImGui::TreeNode(label)) {
		bool enable = settings.enable != 0;
		if (ImGui::Checkbox("Shadow Enable", &enable)) {
			settings.enable = enable;
			changed = true;
		}

		if (!canRender) {
			ImGui::Text("Point shadows are reserved. Use sparingly because they need 6 shadow renders per light.");
		}

		changed |= ImGui::DragFloat("Shadow Bias", &settings.bias, 0.0001f, 0.0f, 0.05f, "%.5f");
		changed |= ImGui::DragFloat("Normal Bias", &settings.normalBias, 0.001f, 0.0f, 0.2f, "%.4f");
		changed |= ImGui::DragFloat("Shadow Strength", &settings.strength, 0.01f, 0.0f, 1.0f);
		if (showDirectionalCameraSettings) {
			ImGui::SeparatorText("Directional Shadow Camera");
			changed |= ImGui::DragFloat3("Target Center", &settings.target.x, 0.1f);
			changed |= ImGui::DragFloat("Light Distance", &settings.distance, 0.5f, 1.0f, 1000.0f);
			changed |= ImGui::DragFloat("Orthographic Size", &settings.orthographicSize, 0.5f, 1.0f, 1000.0f);
			changed |= ImGui::DragFloat("Near Clip", &settings.nearClip, 0.01f, 0.001f, 1000.0f);
			changed |= ImGui::DragFloat("Far Clip", &settings.farClip, 0.5f, 1.0f, 5000.0f);
			changed |= ImGui::Checkbox("Texel Snap", &settings.texelSnap);
			const ShadowSettings beforeClamp = settings;
			settings.distance = (std::max)(settings.distance, 1.0f);
			settings.orthographicSize = (std::max)(settings.orthographicSize, 1.0f);
			settings.nearClip = (std::max)(settings.nearClip, 0.001f);
			settings.farClip = (std::max)(settings.farClip, settings.nearClip + 0.001f);
			changed |=
				beforeClamp.distance != settings.distance ||
				beforeClamp.orthographicSize != settings.orthographicSize ||
				beforeClamp.nearClip != settings.nearClip ||
				beforeClamp.farClip != settings.farClip;
		}

		ImGui::TreePop();
	}
	return changed;
}

void LightManager::DrawImGui() {
	if (!lightingData_) {
		return;
	}

	bool changed = false;

	ImGui::Begin("Light Manager");

	ImGui::Text("Json: %s", jsonPath_.empty() ? "(none)" : jsonPath_.c_str());

	if (ImGui::CollapsingHeader("Shadow Quality", ImGuiTreeNodeFlags_DefaultOpen)) {
		const char* labels[] = {
			"1024",
			"2048",
			"4096"
		};
		int currentIndex = ShadowMapSizeOptionIndex(shadowMapSize_);
		if (ImGui::Combo("Shadow Map Size", &currentIndex, labels, _countof(labels))) {
			SetShadowMapSize(kShadowMapOptions[currentIndex]);
			changed = true;
		}

		const double estimatedBytes =
			static_cast<double>(shadowMapSize_) *
			static_cast<double>(shadowMapSize_) *
			5.0 *
			4.0;
		ImGui::Text(
			"Texture2DArray: %u x %u x 5 / %.1f MB",
			shadowMapSize_,
			shadowMapSize_,
			estimatedBytes / (1024.0 * 1024.0)
		);
		ImGui::TextDisabled("Use a smaller Orthographic Size for sharper directional shadows.");
	}

	if (ImGui::Button("Load")) {
		if (!jsonPath_.empty()) {
			LoadFromJson(jsonPath_);
		}
	}

	ImGui::SameLine();

	if (ImGui::Button("Save")) {
		if (!jsonPath_.empty()) {
			SaveToJson(jsonPath_);
		}
	}

	ImGui::SameLine();

	if (ImGui::Button("Reset")) {
		Reset();
		changed = true;
	}

	if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
		bool enable = directionalLight_.enable != 0;
		if (ImGui::Checkbox("Directional Enable", &enable)) {
			directionalLight_.enable = enable;
			changed = true;
		}

		changed |= ImGui::ColorEdit3("Directional Color", &directionalLight_.color.x);
		if (ImGui::DragFloat3("Directional Direction", &directionalLight_.direction.x, 0.01f, -1.0f, 1.0f)) {
			if (Math::Length(directionalLight_.direction) > 0.000001f) {
				directionalLight_.direction = Math::Normalize(directionalLight_.direction);
			}
			else {
				directionalLight_.direction = { 0.0f, -1.0f, 0.0f };
			}

			changed = true;
		}

		ImGui::Text("Directional Direction Length: %.3f", Math::Length(directionalLight_.direction));
		changed |= ImGui::DragFloat("Directional Intensity", &directionalLight_.intensity, 0.05f, 0.0f, 20.0f);
		changed |= DrawShadowSettingsImGui(
			"Directional Shadow",
			directionalShadowSettings_,
			true,
			true
		);
	}

	if (ImGui::CollapsingHeader("Point Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Button("Add Point Light")) {
			AddPointLight(MakeDefaultPointLight());
			changed = true;
		}

		ImGui::SameLine();

		if (ImGui::Button("Clear Point Lights")) {
			ClearPointLights();
			changed = true;
		}

		for (size_t i = 0; i < pointLights_.size(); ++i) {
			ImGui::PushID(static_cast<int>(i));

			std::string label = "Point Light " + std::to_string(i);
			if (ImGui::TreeNode(label.c_str())) {
				PointLight& light = pointLights_[i];

				bool enable = light.enable != 0;
				if (ImGui::Checkbox("Enable", &enable)) {
					light.enable = enable;
					changed = true;
				}

				changed |= ImGui::ColorEdit3("Color", &light.color.x);
				changed |= ImGui::DragFloat3("Position", &light.position.x, 0.1f);
				changed |= ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 30.0f);
				changed |= ImGui::DragFloat("Radius", &light.radius, 0.1f, 0.1f, 100.0f);
				changed |= ImGui::DragFloat("Decay", &light.decay, 0.05f, 0.0f, 10.0f);
				if (i < pointShadowSettings_.size()) {
					changed |= DrawShadowSettingsImGui("Point Shadow", pointShadowSettings_[i], false);
				}

				if (ImGui::Button("Remove")) {
					RemovePointLight(i);
					changed = true;
					ImGui::TreePop();
					ImGui::PopID();
					break;
				}

				ImGui::TreePop();
			}

			ImGui::PopID();
		}
	}

	if (ImGui::CollapsingHeader("Spot Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Button("Add Spot Light")) {
			AddSpotLight(MakeDefaultSpotLight());
			changed = true;
		}

		ImGui::SameLine();

		if (ImGui::Button("Clear Spot Lights")) {
			ClearSpotLights();
			changed = true;
		}

		for (size_t i = 0; i < spotLights_.size(); ++i) {
			ImGui::PushID(static_cast<int>(i));

			std::string label = "Spot Light " + std::to_string(i);
			if (ImGui::TreeNode(label.c_str())) {
				SpotLight& light = spotLights_[i];

				bool enable = light.enable != 0;
				if (ImGui::Checkbox("Enable", &enable)) {
					light.enable = enable;
					changed = true;
				}

				changed |= ImGui::ColorEdit3("Color", &light.color.x);
				changed |= ImGui::DragFloat3("Position", &light.position.x, 0.1f);
				if (ImGui::DragFloat3("Direction", &light.direction.x, 0.01f, -1.0f, 1.0f)) {
					if (Math::Length(light.direction) > 0.000001f) {
						light.direction = Math::Normalize(light.direction);
					}
					else {
						light.direction = { 0.0f, -1.0f, 0.0f };
					}

					changed = true;
				}

				ImGui::Text("Direction Length: %.3f", Math::Length(light.direction));
				changed |= ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 30.0f);
				changed |= ImGui::DragFloat("Distance", &light.distance, 0.1f, 0.1f, 100.0f);
				changed |= ImGui::DragFloat("Decay", &light.decay, 0.05f, 0.0f, 10.0f);
				changed |= ImGui::DragFloat("CosAngle", &light.cosAngle, 0.01f, -1.0f, 1.0f);
				changed |= ImGui::DragFloat("CosFalloffStart", &light.cosFalloffStart, 0.01f, -1.0f, 1.0f);
				if (i < spotShadowSettings_.size()) {
					changed |= DrawShadowSettingsImGui("Spot Shadow", spotShadowSettings_[i], true);
				}

				if (light.cosFalloffStart < light.cosAngle) {
					light.cosFalloffStart = light.cosAngle;
					changed = true;
				}

				if (ImGui::Button("Remove")) {
					RemoveSpotLight(i);
					changed = true;
					ImGui::TreePop();
					ImGui::PopID();
					break;
				}

				ImGui::TreePop();
			}

			ImGui::PopID();
		}
	}

	ImGui::End();

	if (changed) {
		SyncToGPU();
	}
}
