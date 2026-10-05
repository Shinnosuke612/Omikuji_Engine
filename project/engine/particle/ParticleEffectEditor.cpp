// 役割: パーティクルエフェクトの編集UIと保存操作を実装する。
#include "ParticleEffectEditor.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "../externals/imgui/imgui.h"
#include "../utility/EditableResourcePath.h"
#include "../utility/ResourceTextureCatalog.h"
#include "../utility/StringUtility.h"
#include "ParticleEmitter.h"
#include "ParticleManager.h"

namespace {

	void CopyText(char* destination, size_t destinationSize, const std::string& source) {
		if (destination == nullptr || destinationSize == 0) {
			return;
		}

		strncpy_s(destination, destinationSize, source.c_str(), _TRUNCATE);
	}

void DisableGpuParticlePreview() {
	ParticleManager* particleManager = ParticleManager::GetInstance();
	particleManager->ClearGpuParticlePreview();
}

bool CollapseMaxToMin(float& minValue, float& maxValue) {
	if (maxValue < minValue) {
		maxValue = minValue;
		return true;
	}
	return false;
}

void DragVector3(const char* label, Vector3& value, float speed = 0.01f) {
	ImGui::DragFloat3(label, &value.x, speed);
}

void DragVector4(const char* label, Vector4& value, float speed = 0.01f) {
	ImGui::DragFloat4(label, &value.x, speed, 0.0f, 1.0f);
}

bool ComboBlendMode(const char* label, ParticleCommon::BlendMode& mode) {
	const char* items[] = { "None", "Normal", "Add", "Subtract", "Multiply", "Screen" };
	int current = static_cast<int>(mode);

	if (ImGui::Combo(label, &current, items, IM_ARRAYSIZE(items))) {
		mode = static_cast<ParticleCommon::BlendMode>(current);
		return true;
	}
	return false;
}

bool ComboSimulationType(const char* label, ParticleSimulationType& type) {
	const char* items[] = { "CPU Particle", "GPU Particle" };
	int current = type == ParticleSimulationType::kGPU ? 1 : 0;
	if (ImGui::Combo(label, &current, items, IM_ARRAYSIZE(items))) {
		type = current == 1
			? ParticleSimulationType::kGPU
			: ParticleSimulationType::kCPU;
		return true;
	}
	return false;
}

void DrawGpuWorkInProgressNotices(const ParticleEffectDesc& effect) {
	bool hasNotice = false;
	auto notice = [&hasNotice](const char* text) {
		if (!hasNotice) {
			ImGui::SeparatorText("GPU Work In Progress");
			ImGui::TextColored(
				ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
				"Some saved settings are not fully reproduced by GPU Particle yet."
			);
			hasNotice = true;
		}
		ImGui::BulletText("%s", text);
	};

	if (effect.behavior.motion.wind.enabled &&
		(effect.behavior.motion.wind.turbulenceStrength != 0.0f ||
		 effect.behavior.motion.wind.enableBoundaryFalloff ||
		 effect.behavior.motion.wind.recoverOutsideField)) {
		notice("Wind is approximated on GPU; boundary falloff/recovery/turbulence are not exact yet.");
	}
	if (effect.behavior.color.mode == ParticleManager::ColorChangeMode::kRandomLoop) {
		notice("RandomLoop color is saved, but GPU preview does not apply it yet.");
	}
	if (effect.behavior.render.primitiveType != ParticleManager::PrimitiveType::kPlane) {
		notice("Ring/Cylinder primitives are saved, but GPU currently renders plane particles.");
	}
	if (effect.behavior.render.cullMode != ParticleCommon::CullMode::kNone) {
		notice("GPU Particle currently uses no culling.");
	}
	if (!effect.behavior.render.depthTest || effect.behavior.render.depthWrite) {
		notice("GPU Particle currently uses depth test on and depth write off.");
	}
	if (effect.lightning.enabled) {
		notice("Lightning is saved in the effect file, but it is not part of GPU Particle simulation.");
	}
}

void ComboColorMode(const char* label, ParticleManager::ColorChangeMode& mode) {
	const char* items[] = { "Constant", "OverLife", "RandomLoop" };
	int current = static_cast<int>(mode);

	if (ImGui::Combo(label, &current, items, IM_ARRAYSIZE(items))) {
		mode = static_cast<ParticleManager::ColorChangeMode>(current);
	}
}

void ComboMovementMode(const char* label, ParticleManager::MovementMode& mode) {
	const char* items[] = { "Linear", "VortexInward" };
	int current = static_cast<int>(mode);

	if (ImGui::Combo(label, &current, items, IM_ARRAYSIZE(items))) {
		mode = static_cast<ParticleManager::MovementMode>(current);
	}
}

void ComboVortexAxis(const char* label, ParticleManager::VortexAxis& axis) {
	const char* items[] = { "X", "Y", "Z" };
	int current = static_cast<int>(axis);

	if (ImGui::Combo(label, &current, items, IM_ARRAYSIZE(items))) {
		axis = static_cast<ParticleManager::VortexAxis>(current);
	}
}

void ComboAlignmentAxis(const char* label, ParticleManager::ParticleAlignmentAxis& axis) {
	const char* items[] = { "X", "Y", "Z" };
	int current = static_cast<int>(axis);

	if (ImGui::Combo(label, &current, items, IM_ARRAYSIZE(items))) {
		axis = static_cast<ParticleManager::ParticleAlignmentAxis>(current);
	}
}

void DirectionButton(const char* label, Vector3& direction, const Vector3& value) {
	if (ImGui::Button(label)) {
		direction = value;
	}
}

void ComboBillboardMode(const char* label, ParticleManager::BillboardMode& mode) {
	const char* items[] = { "None", "Billboard" };
	int current = static_cast<int>(mode);

	if (ImGui::Combo(label, &current, items, IM_ARRAYSIZE(items))) {
		mode = static_cast<ParticleManager::BillboardMode>(current);
	}
}

void ComboPrimitiveType(const char* label, ParticleManager::PrimitiveType& type) {
	const char* items[] = { "Plane", "Ring", "Cylinder" };
	int current = static_cast<int>(type);
	if (ImGui::Combo(label, &current, items, IM_ARRAYSIZE(items))) {
		type = static_cast<ParticleManager::PrimitiveType>(current);
	}
}

void ComboCullMode(const char* label, ParticleCommon::CullMode& mode) {
	const char* items[] = { "None", "Back", "Front" };
	int current = static_cast<int>(mode);
	if (ImGui::Combo(label, &current, items, IM_ARRAYSIZE(items))) {
		mode = static_cast<ParticleCommon::CullMode>(current);
	}
}

void ComboRingUvMode(const char* label, ParticleManager::RingUvMode& mode) {
	const char* items[] = { "Horizontal", "Vertical" };
	int current = static_cast<int>(mode);
	if (ImGui::Combo(label, &current, items, IM_ARRAYSIZE(items))) {
		mode = static_cast<ParticleManager::RingUvMode>(current);
	}
}

} // namespace

void ParticleEffectEditor::Initialize(const ParticleEffectDesc& effect, const std::string& filePath) {
	CopyText(filePath_, sizeof(filePath_), filePath);
	CopyStringsFromEffect(effect);
	RefreshEffectFiles();
	RefreshTextureFiles();
	initialized_ = true;
}

void ParticleEffectEditor::RefreshTextureFiles() {
	textureFilePaths_ = ResourceTextureCatalog::Collect();
}

void ParticleEffectEditor::CopyStringsFromEffect(const ParticleEffectDesc& effect) {
	CopyText(name_, sizeof(name_), effect.name);
	CopyText(textureFilePath_, sizeof(textureFilePath_), effect.textureFilePath);
}

void ParticleEffectEditor::CopyStringsToEffect(ParticleEffectDesc& effect) {
	effect.name = name_;
	effect.textureFilePath = textureFilePath_;
}

void ParticleEffectEditor::RefreshEffectFiles() {
	effectFilePaths_.clear();
	effectFileNames_.clear();
	selectedEffectIndex_ = -1;

	const std::filesystem::path currentPath = StringUtility::ToPath(filePath_);
	std::filesystem::path directory = currentPath.parent_path();
	if (directory.empty()) {
		directory = "resources/particles";
	}

	std::error_code error;
	const std::filesystem::path resolvedDirectory =
		EditableResourcePath::Resolve(directory);
	for (const std::filesystem::directory_entry& entry :
		std::filesystem::directory_iterator(resolvedDirectory, error)) {
		if (error) {
			break;
		}
		if (!entry.is_regular_file() || entry.path().extension() != ".json") {
			continue;
		}
		if (entry.path().filename() == "scene_particles.json") {
			continue;
		}

		effectFilePaths_.push_back(
			StringUtility::ToUtf8(
				EditableResourcePath::ToProjectRelative(entry.path())
			)
		);
	}

	std::sort(effectFilePaths_.begin(), effectFilePaths_.end());
	for (size_t index = 0; index < effectFilePaths_.size(); ++index) {
		const std::filesystem::path path =
			StringUtility::ToPath(effectFilePaths_[index]);
		effectFileNames_.push_back(StringUtility::ToUtf8(path.filename()));

		if (path.lexically_normal() == currentPath.lexically_normal()) {
			selectedEffectIndex_ = static_cast<int>(index);
		}
	}
}

bool ParticleEffectEditor::LoadEffectFile(
	const std::string& filePath,
	ParticleEffectDesc& effect,
	ParticleEmitter*& previewEmitter
) {
	if (!ParticleEffectResource::Load(filePath, effect)) {
		return false;
	}

	CopyText(filePath_, sizeof(filePath_), filePath);
	CopyStringsFromEffect(effect);

	delete previewEmitter;
	previewEmitter = ParticleEffectResource::CreateEmitter(effect);
	if (effect.simulationType == ParticleSimulationType::kGPU) {
		ParticleManager::GetInstance()->ApplyGpuParticleEffect(effect);
	} else {
		DisableGpuParticlePreview();
	}
	return true;
}

bool ParticleEffectEditor::DrawImGui(
	ParticleEffectDesc& effect,
	ParticleEmitter*& previewEmitter,
	const char* windowTitle
) {
	if (!initialized_) {
		Initialize(effect, "resources/particles/newParticle.json");
	}

	bool applied = false;
	bool changed = false;

	ImGui::Begin(
		windowTitle,
		nullptr,
		ImGuiWindowFlags_NoFocusOnAppearing
	);

	const char* selectedName =
		selectedEffectIndex_ >= 0 &&
		selectedEffectIndex_ < static_cast<int>(effectFileNames_.size())
		? effectFileNames_[selectedEffectIndex_].c_str()
		: "(select effect)";

	if (ImGui::BeginCombo("Existing Effect", selectedName)) {
		for (int index = 0; index < static_cast<int>(effectFileNames_.size()); ++index) {
			const bool isSelected = selectedEffectIndex_ == index;
			if (ImGui::Selectable(effectFileNames_[index].c_str(), isSelected)) {
				if (LoadEffectFile(effectFilePaths_[index], effect, previewEmitter)) {
					selectedEffectIndex_ = index;
					applied = true;
				}
			}
			if (isSelected) {
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::Button("Refresh List")) {
		RefreshEffectFiles();
	}

	ImGui::InputText("FilePath", filePath_, sizeof(filePath_));
	bool resourceChanged = false;

	resourceChanged |= ImGui::InputText("Name", name_, sizeof(name_));
	resourceChanged |= ImGui::InputText("Texture", textureFilePath_, sizeof(textureFilePath_));
	const char* selectedTexture = textureFilePath_[0] != '\0'
		? textureFilePath_
		: "(select texture)";
	if (ImGui::BeginCombo("Resource Texture", selectedTexture)) {
		for (const std::string& texturePath : textureFilePaths_) {
			const bool selected = texturePath == textureFilePath_;
			if (ImGui::Selectable(texturePath.c_str(), selected)) {
				CopyText(textureFilePath_, sizeof(textureFilePath_), texturePath);
				resourceChanged = true;
			}
			if (selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::Button("Refresh Textures")) {
		RefreshTextureFiles();
	}

	CopyStringsToEffect(effect);
	if (resourceChanged) {
		changed = true;
	}

	if (ComboSimulationType("Simulation", effect.simulationType)) {
		changed = true;
		delete previewEmitter;
		previewEmitter = effect.simulationType == ParticleSimulationType::kCPU
			? ParticleEffectResource::CreateEmitter(effect)
			: nullptr;
		if (effect.simulationType == ParticleSimulationType::kGPU) {
			ParticleManager::GetInstance()->ApplyGpuParticleEffect(effect);
		} else {
			DisableGpuParticlePreview();
		}
	}
	if (effect.simulationType == ParticleSimulationType::kGPU) {
		if (ImGui::CollapsingHeader("GPU Runtime", ImGuiTreeNodeFlags_DefaultOpen)) {
			ParticleManager* particleManager = ParticleManager::GetInstance();
			bool gpuParticleEnabled = particleManager->IsGpuParticleEnabled();
			if (ImGui::Checkbox("GPU Particle Enabled", &gpuParticleEnabled)) {
				particleManager->SetGpuParticleEnabled(gpuParticleEnabled);
			}
			ImGui::SameLine();
			if (ImGui::Button("Apply GPU Preview")) {
				applied = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Reset GPU Buffer")) {
				particleManager->RequestGpuParticleReset();
			}
			ImGui::SameLine();
			if (ImGui::Button("Emit Once")) {
				particleManager->EmitGpuParticleOnce();
			}
			DrawGpuWorkInProgressNotices(effect);
		}
	}

	changed |= ComboBlendMode("BlendMode", effect.blendMode);

	if (ImGui::CollapsingHeader("Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
		changed |= ImGui::DragFloat3("Translate", &effect.emitter.translate.x, 0.05f);
		changed |= ImGui::DragFloat3("SpawnSize", &effect.emitter.spawnSize.x, 0.05f);

		int count = static_cast<int>(effect.emitter.count);
		if (ImGui::DragInt("Count", &count, 1, 0, 1000)) {
			effect.emitter.count = static_cast<uint32_t>(count < 0 ? 0 : count);
			changed = true;
		}

		if (!effect.behavior.life.isLooping) {
			changed |= ImGui::DragFloat(
				"Frequency",
				&effect.emitter.frequency,
				0.001f,
				1.0f / 60.0f,
				10.0f
			);
			if (effect.emitter.frequency <= 0.0f) {
				effect.emitter.frequency = 1.0f / 60.0f;
				changed = true;
			}
		}
		changed |= ImGui::Checkbox("Active", &effect.emitter.isActive);
	}

	if (ImGui::CollapsingHeader("Lightning")) {
		changed |= ImGui::Checkbox("Enable Lightning", &effect.lightning.enabled);
		if (effect.lightning.enabled) {
			ImGui::TextDisabled("Offsets are relative to the emitter position.");
			changed |= ImGui::DragFloat3(
				"Start Offset",
				&effect.lightning.startOffset.x,
				0.05f
			);
			changed |= ImGui::DragFloat3(
				"End Offset",
				&effect.lightning.endOffset.x,
				0.05f
			);
			changed |= ImGui::DragFloat3(
				"Random Range",
				&effect.lightning.randomRange.x,
				0.05f,
				0.0f,
				100.0f
			);
			changed |= ImGui::ColorEdit4(
				"Core Color",
				&effect.lightning.coreColor.x,
				ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR
			);
			changed |= ImGui::ColorEdit4(
				"Branch Color",
				&effect.lightning.branchColor.x,
				ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR
			);
			changed |= ImGui::DragFloat(
				"Thickness",
				&effect.lightning.thickness,
				0.001f,
				0.001f,
				1.0f,
				"%.3f"
			);
			changed |= ImGui::DragFloat(
				"Duration",
				&effect.lightning.duration,
				0.01f,
				0.01f,
				5.0f
			);
			changed |= ImGui::DragFloat(
				"Jitter",
				&effect.lightning.jitter,
				0.01f,
				0.0f,
				10.0f
			);
			changed |= ImGui::DragFloat(
				"Branch Length",
				&effect.lightning.branchLength,
				0.01f,
				0.0f,
				10.0f
			);
			changed |= ImGui::SliderFloat(
				"Branch Probability",
				&effect.lightning.branchProbability,
				0.0f,
				1.0f
			);
			int segmentCount = static_cast<int>(effect.lightning.segmentCount);
			if (ImGui::SliderInt("Segments", &segmentCount, 1, 64)) {
				effect.lightning.segmentCount =
					static_cast<uint32_t>(std::clamp(segmentCount, 1, 64));
				changed = true;
			}

			ImGui::SeparatorText("Exposure Flash");
			changed |= ImGui::Checkbox(
				"Flash Exposure",
				&effect.lightning.flashExposure
			);
			if (effect.lightning.flashExposure) {
				changed |= ImGui::DragFloat(
					"Flash Exposure Value",
					&effect.lightning.flashExposureValue,
					0.01f,
					0.01f,
					20.0f
				);
				changed |= ImGui::DragFloat(
					"Return Speed",
					&effect.lightning.flashReturnSpeed,
					0.01f,
					0.01f,
					100.0f
				);
			}
		}
	}

	if (ImGui::CollapsingHeader("Life", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Loop", &effect.behavior.life.isLooping);
		if (effect.behavior.life.isLooping) {
			ImGui::DragFloat(
				"LoopDuration",
				&effect.behavior.life.loopDuration,
				0.01f,
				0.001f,
				100.0f
			);
			ImGui::Checkbox(
				"LoopPingPong",
				&effect.behavior.life.loopPingPong
			);
		}
		else {
			ImGui::DragFloat("LifeTimeMin", &effect.behavior.life.lifeTimeMin, 0.01f, 0.0f, 100.0f);
			ImGui::DragFloat("LifeTimeMax", &effect.behavior.life.lifeTimeMax, 0.01f, 0.0f, 100.0f);
			ImGui::Checkbox("EnableLifeFade", &effect.behavior.life.enableLifeFade);
			ImGui::DragFloat("FadeOutStartRatio", &effect.behavior.life.fadeOutStartRatio, 0.01f, 0.0f, 0.99f);
		}
	}

	if (ImGui::CollapsingHeader("Scale", ImGuiTreeNodeFlags_DefaultOpen)) {
		DragVector3("StartScaleMin", effect.behavior.scale.startScaleMin, 0.01f);
		DragVector3("StartScaleMax", effect.behavior.scale.startScaleMax, 0.01f);

		ImGui::Checkbox("EnableScaleOverLife", &effect.behavior.scale.enableScaleOverLife);

		DragVector3("EndScaleMin", effect.behavior.scale.endScaleMin, 0.01f);
		DragVector3("EndScaleMax", effect.behavior.scale.endScaleMax, 0.01f);
	}

	if (ImGui::CollapsingHeader("Rotation", ImGuiTreeNodeFlags_DefaultOpen)) {
		DragVector3("InitialRotationMin", effect.behavior.rotation.initialRotationMin, 0.01f);
		DragVector3("InitialRotationMax", effect.behavior.rotation.initialRotationMax, 0.01f);
		ImGui::Checkbox(
			"RotationOverTime",
			&effect.behavior.rotation.enableRotationOverTime
		);
		if (effect.behavior.rotation.enableRotationOverTime) {
			DragVector3(
				"RotationSpeed",
				effect.behavior.rotation.rotationSpeed,
				0.01f
			);
		}
		ImGui::Checkbox(
			"AlignToVelocity",
			&effect.behavior.rotation.alignToVelocity
		);
		if (effect.behavior.rotation.alignToVelocity) {
			ComboAlignmentAxis("AlignAxis", effect.behavior.rotation.alignAxis);
		}
	}

	if (ImGui::CollapsingHeader("Motion", ImGuiTreeNodeFlags_DefaultOpen)) {
		ComboMovementMode("MovementMode", effect.behavior.motion.mode);

		if (ImGui::TreeNode("Linear")) {
			DragVector3("BaseVelocity", effect.behavior.motion.linear.baseVelocity, 0.05f);
			DragVector3("VelocityRandomRange", effect.behavior.motion.linear.velocityRandomRange, 0.05f);
			ImGui::Checkbox("EnableAcceleration", &effect.behavior.motion.linear.enableAcceleration);
			if (effect.behavior.motion.linear.enableAcceleration) {
				DragVector3("BaseAcceleration", effect.behavior.motion.linear.baseAcceleration, 0.01f);
				DragVector3("AccelerationRandomRange", effect.behavior.motion.linear.accelerationRandomRange, 0.01f);
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Sway")) {
			ImGui::DragFloat("Amplitude", &effect.behavior.motion.sway.amplitude, 0.01f, 0.0f, 100.0f);
			ImGui::DragFloat("Frequency", &effect.behavior.motion.sway.frequency, 0.01f, 0.0f, 100.0f);
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Vortex")) {
			auto& vortex = effect.behavior.motion.vortex;
			ImGui::Checkbox(
				"UseEmitterOffset",
				&vortex.useEmitterOffset
			);
			DragVector3(
				vortex.useEmitterOffset ? "CenterOffset" : "WorldCenter",
				vortex.center,
				0.05f
			);
			ComboVortexAxis("Axis", vortex.axis);

			bool rangeCollapsed = false;
			changed |= ImGui::DragFloat(
				"AngularSpeedMin",
				&vortex.angularSpeedMin,
				0.01f,
				-100.0f,
				100.0f
			);
			changed |= ImGui::DragFloat(
				"AngularSpeedMax",
				&vortex.angularSpeedMax,
				0.01f,
				-100.0f,
				100.0f
			);
			rangeCollapsed |= CollapseMaxToMin(
				vortex.angularSpeedMin,
				vortex.angularSpeedMax
			);

			changed |= ImGui::DragFloat(
				"InwardSpeedMin",
				&vortex.inwardSpeedMin,
				0.01f,
				-100.0f,
				100.0f
			);
			changed |= ImGui::DragFloat(
				"InwardSpeedMax",
				&vortex.inwardSpeedMax,
				0.01f,
				-100.0f,
				100.0f
			);
			rangeCollapsed |= CollapseMaxToMin(
				vortex.inwardSpeedMin,
				vortex.inwardSpeedMax
			);

			changed |= ImGui::DragFloat(
				"VerticalSpeedMin",
				&vortex.verticalSpeedMin,
				0.01f,
				-100.0f,
				100.0f
			);
			changed |= ImGui::DragFloat(
				"VerticalSpeedMax",
				&vortex.verticalSpeedMax,
				0.01f,
				-100.0f,
				100.0f
			);
			rangeCollapsed |= CollapseMaxToMin(
				vortex.verticalSpeedMin,
				vortex.verticalSpeedMax
			);
			if (rangeCollapsed) {
				changed = true;
				ImGui::TextDisabled("Max collapsed to Min for inverted vortex range.");
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Point Field")) {
			auto& field = effect.behavior.motion.pointField;
			ImGui::Checkbox("Enabled##PointField", &field.enabled);
			ImGui::Checkbox("UseEmitterOffset##PointField", &field.useEmitterOffset);
			DragVector3(
				field.useEmitterOffset ? "CenterOffset##PointField" : "WorldCenter##PointField",
				field.center,
				0.05f
			);
			ImGui::DragFloat("Radius##PointField", &field.radius, 0.05f, 0.0f, 10000.0f);
			ImGui::DragFloat("Attraction##PointField", &field.attractionStrength, 0.05f, -1000.0f, 1000.0f);
			ImGui::DragFloat("Repulsion##PointField", &field.repulsionStrength, 0.05f, -1000.0f, 1000.0f);
			ImGui::DragFloat("Orbit##PointField", &field.orbitStrength, 0.05f, -1000.0f, 1000.0f);
			DragVector3("OrbitAxis##PointField", field.orbitAxis, 0.01f);
			DirectionButton("+X##PointField", field.orbitAxis, { 1.0f, 0.0f, 0.0f });
			ImGui::SameLine();
			DirectionButton("+Y##PointField", field.orbitAxis, { 0.0f, 1.0f, 0.0f });
			ImGui::SameLine();
			DirectionButton("+Z##PointField", field.orbitAxis, { 0.0f, 0.0f, 1.0f });
			ImGui::DragFloat("Falloff##PointField", &field.falloff, 0.01f, 0.0f, 16.0f);
			ImGui::DragFloat("Damping##PointField", &field.damping, 0.01f, 0.0f, 100.0f);
			ImGui::TextDisabled("Point Field affects Linear movement velocity.");
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Wind Field")) {
			auto& wind = effect.behavior.motion.wind;

			ImGui::Checkbox("Enabled", &wind.enabled);
			ImGui::Checkbox("UseEmitterOffset", &wind.useEmitterOffset);
			DragVector3(
				wind.useEmitterOffset ? "CenterOffset" : "WorldCenter",
				wind.center,
				0.05f
			);
			DragVector3("FieldSize", wind.size, 0.05f);
			wind.size.x = (std::max)(wind.size.x, 0.0f);
			wind.size.y = (std::max)(wind.size.y, 0.0f);
			wind.size.z = (std::max)(wind.size.z, 0.0f);
			DragVector3("Direction", wind.direction, 0.01f);

			DirectionButton("+X", wind.direction, { 1.0f, 0.0f, 0.0f });
			ImGui::SameLine();
			DirectionButton("-X", wind.direction, { -1.0f, 0.0f, 0.0f });
			ImGui::SameLine();
			DirectionButton("+Y", wind.direction, { 0.0f, 1.0f, 0.0f });
			ImGui::SameLine();
			DirectionButton("-Y", wind.direction, { 0.0f, -1.0f, 0.0f });
			ImGui::SameLine();
			DirectionButton("+Z", wind.direction, { 0.0f, 0.0f, 1.0f });
			ImGui::SameLine();
			DirectionButton("-Z", wind.direction, { 0.0f, 0.0f, -1.0f });

			ImGui::DragFloat("Strength", &wind.strength, 0.05f, -100.0f, 100.0f);
			ImGui::Checkbox("SmoothVelocity", &wind.smoothVelocity);
			if (wind.smoothVelocity) {
				ImGui::DragFloat("Acceleration", &wind.acceleration, 0.05f, 0.0f, 1000.0f);
				ImGui::Checkbox("RecoverOutsideField", &wind.recoverOutsideField);
				if (wind.recoverOutsideField) {
					ImGui::DragFloat("Deceleration", &wind.deceleration, 0.05f, 0.0f, 1000.0f);
				}
			}
			ImGui::Checkbox("BoundaryFalloff", &wind.enableBoundaryFalloff);
			if (wind.enableBoundaryFalloff) {
				ImGui::DragFloat("FalloffDistance", &wind.boundaryFalloff, 0.05f, 0.0f, 1000.0f);
			}
			ImGui::DragFloat("TurbulenceStrength", &wind.turbulenceStrength, 0.01f, 0.0f, 100.0f);
			ImGui::DragFloat("TurbulenceFrequency", &wind.turbulenceFrequency, 0.01f, 0.0f, 100.0f);
			ImGui::DragFloat("TurbulenceScale", &wind.turbulenceScale, 0.001f, 0.0f, 100.0f);

			if (ImGui::Button("Rain Wind Preset")) {
				wind.enabled = true;
				wind.useEmitterOffset = true;
				wind.center = { 0.0f, -15.0f, 0.0f };
				wind.size = { 100.0f, 32.0f, 100.0f };
				wind.direction = { 1.0f, 0.0f, 0.25f };
				wind.strength = 3.0f;
				wind.smoothVelocity = true;
				wind.acceleration = 7.0f;
				wind.recoverOutsideField = true;
				wind.deceleration = 3.5f;
				wind.enableBoundaryFalloff = true;
				wind.boundaryFalloff = 3.0f;
				wind.turbulenceStrength = 0.8f;
				wind.turbulenceFrequency = 2.2f;
				wind.turbulenceScale = 0.06f;
			}

			ImGui::TreePop();
		}
	}

	if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
		ComboColorMode("ColorMode", effect.behavior.color.mode);

		DragVector4("StartColorMin", effect.behavior.color.startColorMin, 0.01f);
		DragVector4("StartColorMax", effect.behavior.color.startColorMax, 0.01f);
		DragVector4("EndColorMin", effect.behavior.color.endColorMin, 0.01f);
		DragVector4("EndColorMax", effect.behavior.color.endColorMax, 0.01f);

		DragVector4("RandomColorMin", effect.behavior.color.randomColorMin, 0.01f);
		DragVector4("RandomColorMax", effect.behavior.color.randomColorMax, 0.01f);

		ImGui::DragFloat("RandomColorIntervalMin", &effect.behavior.color.randomColorChangeIntervalMin, 0.01f, 0.001f, 10.0f);
		ImGui::DragFloat("RandomColorIntervalMax", &effect.behavior.color.randomColorChangeIntervalMax, 0.01f, 0.001f, 10.0f);
		ImGui::DragFloat("RandomColorLerpSpeed", &effect.behavior.color.randomColorLerpSpeed, 0.01f, 0.0f, 100.0f);
	}

	if (ImGui::CollapsingHeader("Render", ImGuiTreeNodeFlags_DefaultOpen)) {
		ComboBillboardMode("BillboardMode", effect.behavior.render.billboardMode);
		ComboPrimitiveType("PrimitiveType", effect.behavior.render.primitiveType);
		ImGui::DragFloat2(
			"UVScrollSpeed",
			&effect.behavior.render.uvScrollSpeed.x,
			0.01f
		);
		ImGui::Checkbox("FlipU", &effect.behavior.render.flipU);
		ImGui::SameLine();
		ImGui::Checkbox("FlipV", &effect.behavior.render.flipV);
		ImGui::SliderFloat(
			"AlphaCutoff",
			&effect.behavior.render.alphaCutoff,
			0.0f,
			1.0f
		);
		ImGui::SliderFloat(
			"Emissive",
			&effect.behavior.render.emissiveIntensity,
			0.0f,
			20.0f
		);
		ComboCullMode("CullMode", effect.behavior.render.cullMode);
		ImGui::Checkbox("DepthTest", &effect.behavior.render.depthTest);
		ImGui::SameLine();
		ImGui::Checkbox("DepthWrite", &effect.behavior.render.depthWrite);

		if (effect.behavior.render.primitiveType == ParticleManager::PrimitiveType::kRing) {
			int divisions = static_cast<int>(effect.behavior.render.ring.divisions);
			if (ImGui::DragInt("RingDivisions", &divisions, 1, 3, 256)) {
				effect.behavior.render.ring.divisions =
					static_cast<uint32_t>(std::clamp(divisions, 3, 256));
			}
			ImGui::DragFloat(
				"OuterRadius",
				&effect.behavior.render.ring.outerRadius,
				0.01f,
				0.001f,
				100.0f
			);
			ImGui::DragFloat(
				"InnerRadius",
				&effect.behavior.render.ring.innerRadius,
				0.01f,
				0.0f,
				100.0f
			);
			ImGui::DragFloat(
				"StartAngle",
				&effect.behavior.render.ring.startAngle,
				0.01f
			);
			ImGui::DragFloat(
				"EndAngle",
				&effect.behavior.render.ring.endAngle,
				0.01f
			);
			ImGui::ColorEdit4(
				"OuterColor",
				&effect.behavior.render.ring.outerColor.x
			);
			ImGui::ColorEdit4(
				"InnerColor",
				&effect.behavior.render.ring.innerColor.x
			);
			ComboRingUvMode("RingUV", effect.behavior.render.ring.uvMode);
		}
		else if (
			effect.behavior.render.primitiveType ==
			ParticleManager::PrimitiveType::kCylinder
		) {
			int divisions = static_cast<int>(effect.behavior.render.cylinder.divisions);
			if (ImGui::DragInt("CylinderDivisions", &divisions, 1, 3, 256)) {
				effect.behavior.render.cylinder.divisions =
					static_cast<uint32_t>(std::clamp(divisions, 3, 256));
			}
			ImGui::DragFloat(
				"TopRadius",
				&effect.behavior.render.cylinder.topRadius,
				0.01f,
				0.0f,
				100.0f
			);
			ImGui::DragFloat(
				"BottomRadius",
				&effect.behavior.render.cylinder.bottomRadius,
				0.01f,
				0.0f,
				100.0f
			);
			ImGui::DragFloat(
				"Height",
				&effect.behavior.render.cylinder.height,
				0.01f,
				0.001f,
				100.0f
			);
			ImGui::DragFloat(
				"CylinderStartAngle",
				&effect.behavior.render.cylinder.startAngle,
				0.01f
			);
			ImGui::DragFloat(
				"CylinderEndAngle",
				&effect.behavior.render.cylinder.endAngle,
				0.01f
			);
			ImGui::ColorEdit4(
				"TopColor",
				&effect.behavior.render.cylinder.topColor.x
			);
			ImGui::ColorEdit4(
				"BottomColor",
				&effect.behavior.render.cylinder.bottomColor.x
			);
			ComboRingUvMode(
				"CylinderUV",
				effect.behavior.render.cylinder.uvMode
			);
		}
	}

	// 数値編集をリアルタイム反映
	if (effect.simulationType == ParticleSimulationType::kGPU) {
		delete previewEmitter;
		previewEmitter = nullptr;
		if (changed || applied) {
			ParticleManager::GetInstance()->ApplyGpuParticleEffect(effect);
		}
	} else if (previewEmitter) {
		if (changed || applied) {
			if (ParticleEffectResource::PrepareParticleGroup(effect, false)) {
				ParticleEffectResource::ApplyToEmitter(*previewEmitter, effect);
			}
		}
	} else {
		previewEmitter = ParticleEffectResource::CreateEmitter(effect);
	}

	if (ImGui::Button("Apply")) {
		if (effect.simulationType == ParticleSimulationType::kCPU) {
			DisableGpuParticlePreview();
		}
		delete previewEmitter;
		previewEmitter = ParticleEffectResource::CreateEmitter(effect);
		applied = true;
	}

	ImGui::SameLine();

	if (ImGui::Button("Save")) {
		const bool saved = ParticleEffectResource::Save(filePath_, effect);
		if (saved) {
			ParticleManager::GetInstance()->RefreshPlacementAssetsForEffect(filePath_);
		}
		persistenceMessage_ = saved
			? "Saved to project resources. Backup updated."
			: "Save failed. The previous file was not overwritten.";
	}

	ImGui::SameLine();

	if (ImGui::Button("Load")) {
		if (LoadEffectFile(filePath_, effect, previewEmitter)) {
			RefreshEffectFiles();
			applied = true;
			persistenceMessage_ = "Loaded from project resources.";
		} else {
			persistenceMessage_ = "Load failed. Current edits were kept.";
		}
	}

	ImGui::TextDisabled(
		"Project file: %s",
		StringUtility::ToUtf8(
			EditableResourcePath::Resolve(StringUtility::ToPath(filePath_))
		).c_str()
	);
	if (!persistenceMessage_.empty()) {
		ImGui::TextWrapped("%s", persistenceMessage_.c_str());
	}

	ImGui::End();

	return applied;
}
