// 役割: パーティクルエフェクトJSONの読み書きと既定値補完を実装する。
#include "ParticleEffectResource.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "../externals/nlohmann/json.hpp"
#include "../utility/EditableResourcePath.h"

using json = nlohmann::json;

namespace {

constexpr float kMinEmitterFrequency = 1.0f / 60.0f;

float NormalizeEmitterFrequency(float frequency) {
	return frequency > 0.0f ? frequency : kMinEmitterFrequency;
}

json ToJson(const Vector3& v) {
	return json::array({ v.x, v.y, v.z });
}

json ToJson(const Vector4& v) {
	return json::array({ v.x, v.y, v.z, v.w });
}

Vector3 ReadVector3(const json& j, const Vector3& defaultValue) {
	if (!j.is_array() || j.size() < 3) {
		return defaultValue;
	}

	return {
		j.at(0).get<float>(),
		j.at(1).get<float>(),
		j.at(2).get<float>()
	};
}

Vector4 ReadVector4(const json& j, const Vector4& defaultValue) {
	if (!j.is_array() || j.size() < 4) {
		return defaultValue;
	}

	return {
		j.at(0).get<float>(),
		j.at(1).get<float>(),
		j.at(2).get<float>(),
		j.at(3).get<float>()
	};
}

std::string ToString(ParticleCommon::BlendMode mode) {
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

std::string ToString(ParticleSimulationType type) {
	switch (type) {
	case ParticleSimulationType::kGPU: return "GPU";
	case ParticleSimulationType::kCPU:
	default:
		return "CPU";
	}
}

ParticleSimulationType ToSimulationType(const std::string& text) {
	if (text == "GPU" || text == "Gpu" || text == "gpu") {
		return ParticleSimulationType::kGPU;
	}
	return ParticleSimulationType::kCPU;
}

std::string ToString(ParticleManager::ColorChangeMode mode) {
	switch (mode) {
	case ParticleManager::ColorChangeMode::kConstant: return "Constant";
	case ParticleManager::ColorChangeMode::kOverLife: return "OverLife";
	case ParticleManager::ColorChangeMode::kRandomLoop: return "RandomLoop";
	default: return "Constant";
	}
}

ParticleManager::ColorChangeMode ToColorChangeMode(const std::string& text) {
	if (text == "Constant") return ParticleManager::ColorChangeMode::kConstant;
	if (text == "OverLife") return ParticleManager::ColorChangeMode::kOverLife;
	if (text == "RandomLoop") return ParticleManager::ColorChangeMode::kRandomLoop;

	return ParticleManager::ColorChangeMode::kConstant;
}

std::string ToString(ParticleManager::MovementMode mode) {
	switch (mode) {
	case ParticleManager::MovementMode::kLinear: return "Linear";
	case ParticleManager::MovementMode::kVortexInward: return "VortexInward";
	default: return "Linear";
	}
}

ParticleManager::MovementMode ToMovementMode(const std::string& text) {
	if (text == "Linear") return ParticleManager::MovementMode::kLinear;
	if (text == "VortexInward") return ParticleManager::MovementMode::kVortexInward;

	return ParticleManager::MovementMode::kLinear;
}

std::string ToString(ParticleManager::VortexAxis axis) {
	switch (axis) {
	case ParticleManager::VortexAxis::kX: return "X";
	case ParticleManager::VortexAxis::kY: return "Y";
	case ParticleManager::VortexAxis::kZ: return "Z";
	default: return "Y";
	}
}

ParticleManager::VortexAxis ToVortexAxis(const std::string& text) {
	if (text == "X") return ParticleManager::VortexAxis::kX;
	if (text == "Y") return ParticleManager::VortexAxis::kY;
	if (text == "Z") return ParticleManager::VortexAxis::kZ;

	return ParticleManager::VortexAxis::kY;
}

void CollapseMaxToMin(float& minValue, float& maxValue) {
	if (maxValue < minValue) {
		maxValue = minValue;
	}
}

std::string ToString(ParticleManager::ParticleAlignmentAxis axis) {
	switch (axis) {
	case ParticleManager::ParticleAlignmentAxis::kX: return "X";
	case ParticleManager::ParticleAlignmentAxis::kY: return "Y";
	case ParticleManager::ParticleAlignmentAxis::kZ: return "Z";
	}

	return "Y";
}

ParticleManager::ParticleAlignmentAxis ToParticleAlignmentAxis(const std::string& text) {
	if (text == "X") return ParticleManager::ParticleAlignmentAxis::kX;
	if (text == "Y") return ParticleManager::ParticleAlignmentAxis::kY;
	if (text == "Z") return ParticleManager::ParticleAlignmentAxis::kZ;

	return ParticleManager::ParticleAlignmentAxis::kY;
}

std::string ToString(ParticleManager::BillboardMode mode) {
	switch (mode) {
	case ParticleManager::BillboardMode::kNone: return "None";
	case ParticleManager::BillboardMode::kBillboard: return "Billboard";
	default: return "Billboard";
	}
}

ParticleManager::BillboardMode ToBillboardMode(const std::string& text) {
	if (text == "None") return ParticleManager::BillboardMode::kNone;
	if (text == "Billboard") return ParticleManager::BillboardMode::kBillboard;

	return ParticleManager::BillboardMode::kBillboard;
}

std::string ToString(ParticleManager::PrimitiveType type) {
	switch (type) {
	case ParticleManager::PrimitiveType::kRing: return "Ring";
	case ParticleManager::PrimitiveType::kCylinder: return "Cylinder";
	default: return "Plane";
	}
}

ParticleManager::PrimitiveType ToPrimitiveType(const std::string& text) {
	if (text == "Ring") return ParticleManager::PrimitiveType::kRing;
	if (text == "Cylinder") return ParticleManager::PrimitiveType::kCylinder;
	return ParticleManager::PrimitiveType::kPlane;
}

std::string ToString(ParticleManager::RingUvMode mode) {
	return mode == ParticleManager::RingUvMode::kVertical ? "Vertical" : "Horizontal";
}

ParticleManager::RingUvMode ToRingUvMode(const std::string& text) {
	return text == "Vertical"
		? ParticleManager::RingUvMode::kVertical
		: ParticleManager::RingUvMode::kHorizontal;
}

std::string ToString(ParticleCommon::CullMode mode) {
	switch (mode) {
	case ParticleCommon::CullMode::kBack: return "Back";
	case ParticleCommon::CullMode::kFront: return "Front";
	default: return "None";
	}
}

ParticleCommon::CullMode ToCullMode(const std::string& text) {
	if (text == "Back") return ParticleCommon::CullMode::kBack;
	if (text == "Front") return ParticleCommon::CullMode::kFront;
	return ParticleCommon::CullMode::kNone;
}

void WriteRender(json& j, const ParticleManager::ParticleRenderDesc& render) {
	j = {
		{ "billboardMode", ToString(render.billboardMode) },
		{ "primitiveType", ToString(render.primitiveType) },
		{ "flipU", render.flipU },
		{ "flipV", render.flipV },
		{ "alphaCutoff", render.alphaCutoff },
		{ "emissiveIntensity", render.emissiveIntensity },
		{ "cullMode", ToString(render.cullMode) },
		{ "depthTest", render.depthTest },
		{ "depthWrite", render.depthWrite },
		{ "uvScrollSpeed", json::array({
			render.uvScrollSpeed.x,
			render.uvScrollSpeed.y
		}) },
		{ "ring", {
			{ "divisions", render.ring.divisions },
			{ "outerRadius", render.ring.outerRadius },
			{ "innerRadius", render.ring.innerRadius },
			{ "startAngle", render.ring.startAngle },
			{ "endAngle", render.ring.endAngle },
			{ "outerColor", ToJson(render.ring.outerColor) },
			{ "innerColor", ToJson(render.ring.innerColor) },
			{ "uvMode", ToString(render.ring.uvMode) }
		} },
		{ "cylinder", {
			{ "divisions", render.cylinder.divisions },
			{ "topRadius", render.cylinder.topRadius },
			{ "bottomRadius", render.cylinder.bottomRadius },
			{ "height", render.cylinder.height },
			{ "startAngle", render.cylinder.startAngle },
			{ "endAngle", render.cylinder.endAngle },
			{ "topColor", ToJson(render.cylinder.topColor) },
			{ "bottomColor", ToJson(render.cylinder.bottomColor) },
			{ "uvMode", ToString(render.cylinder.uvMode) }
		} }
	};
}

void ReadRender(const json& j, ParticleManager::ParticleRenderDesc& render) {
	render.billboardMode =
		ToBillboardMode(j.value("billboardMode", ToString(render.billboardMode)));
	render.primitiveType =
		ToPrimitiveType(j.value("primitiveType", ToString(render.primitiveType)));
	render.flipU = j.value("flipU", render.flipU);
	render.flipV = j.value("flipV", render.flipV);
	render.alphaCutoff =
		std::clamp(j.value("alphaCutoff", render.alphaCutoff), 0.0f, 1.0f);
	render.emissiveIntensity =
		(std::max)(0.0f, j.value("emissiveIntensity", render.emissiveIntensity));
	render.cullMode = ToCullMode(j.value("cullMode", ToString(render.cullMode)));
	render.depthTest = j.value("depthTest", render.depthTest);
	render.depthWrite = j.value("depthWrite", render.depthWrite);

	if (j.contains("uvScrollSpeed")) {
		const json& uvScroll = j.at("uvScrollSpeed");
		if (uvScroll.is_array() && uvScroll.size() >= 2) {
			render.uvScrollSpeed = {
				uvScroll.at(0).get<float>(),
				uvScroll.at(1).get<float>()
			};
		}
	}

	if (j.contains("ring")) {
		const json& ring = j.at("ring");
		render.ring.divisions =
			std::clamp(ring.value("divisions", render.ring.divisions), 3u, 256u);
		render.ring.outerRadius =
			(std::max)(ring.value("outerRadius", render.ring.outerRadius), 0.001f);
		render.ring.innerRadius = std::clamp(
			ring.value("innerRadius", render.ring.innerRadius),
			0.0f,
			render.ring.outerRadius
		);
		render.ring.startAngle = ring.value("startAngle", render.ring.startAngle);
		render.ring.endAngle = ring.value("endAngle", render.ring.endAngle);

		if (ring.contains("outerColor")) {
			render.ring.outerColor =
				ReadVector4(ring.at("outerColor"), render.ring.outerColor);
		}
		if (ring.contains("innerColor")) {
			render.ring.innerColor =
				ReadVector4(ring.at("innerColor"), render.ring.innerColor);
		}

		render.ring.uvMode =
			ToRingUvMode(ring.value("uvMode", ToString(render.ring.uvMode)));
	}

	if (j.contains("cylinder")) {
		const json& cylinder = j.at("cylinder");
		render.cylinder.divisions = std::clamp(
			cylinder.value("divisions", render.cylinder.divisions),
			3u,
			256u
		);
		render.cylinder.topRadius =
			(std::max)(cylinder.value("topRadius", render.cylinder.topRadius), 0.0f);
		render.cylinder.bottomRadius =
			(std::max)(cylinder.value("bottomRadius", render.cylinder.bottomRadius), 0.0f);
		render.cylinder.height =
			(std::max)(cylinder.value("height", render.cylinder.height), 0.001f);
		render.cylinder.startAngle =
			cylinder.value("startAngle", render.cylinder.startAngle);
		render.cylinder.endAngle =
			cylinder.value("endAngle", render.cylinder.endAngle);

		if (cylinder.contains("topColor")) {
			render.cylinder.topColor =
				ReadVector4(cylinder.at("topColor"), render.cylinder.topColor);
		}
		if (cylinder.contains("bottomColor")) {
			render.cylinder.bottomColor =
				ReadVector4(cylinder.at("bottomColor"), render.cylinder.bottomColor);
		}

		render.cylinder.uvMode = ToRingUvMode(
			cylinder.value("uvMode", ToString(render.cylinder.uvMode))
		);
	}
}

void WriteBehavior(json& j, const ParticleManager::ParticleBehavior& b) {
	j["life"] = {
		{ "isLooping", b.life.isLooping },
		{ "loopDuration", b.life.loopDuration },
		{ "loopPingPong", b.life.loopPingPong },
		{ "lifeTimeMin", b.life.lifeTimeMin },
		{ "lifeTimeMax", b.life.lifeTimeMax },
		{ "enableLifeFade", b.life.enableLifeFade },
		{ "fadeOutStartRatio", b.life.fadeOutStartRatio }
	};

	j["scale"] = {
		{ "startScaleMin", ToJson(b.scale.startScaleMin) },
		{ "startScaleMax", ToJson(b.scale.startScaleMax) },
		{ "enableScaleOverLife", b.scale.enableScaleOverLife },
		{ "endScaleMin", ToJson(b.scale.endScaleMin) },
		{ "endScaleMax", ToJson(b.scale.endScaleMax) }
	};

	j["rotation"] = {
		{ "initialRotationMin", ToJson(b.rotation.initialRotationMin) },
		{ "initialRotationMax", ToJson(b.rotation.initialRotationMax) },
		{ "enableRotationOverTime", b.rotation.enableRotationOverTime },
		{ "rotationSpeed", ToJson(b.rotation.rotationSpeed) },
		{ "alignToVelocity", b.rotation.alignToVelocity },
		{ "alignAxis", ToString(b.rotation.alignAxis) }
	};

	j["motion"]["mode"] = ToString(b.motion.mode);

	j["motion"]["linear"] = {
		{ "baseVelocity", ToJson(b.motion.linear.baseVelocity) },
		{ "velocityRandomRange", ToJson(b.motion.linear.velocityRandomRange) },
		{ "enableAcceleration", b.motion.linear.enableAcceleration },
		{ "baseAcceleration", ToJson(b.motion.linear.baseAcceleration) },
		{ "accelerationRandomRange", ToJson(b.motion.linear.accelerationRandomRange) }
	};

	j["motion"]["sway"] = {
		{ "amplitude", b.motion.sway.amplitude },
		{ "frequency", b.motion.sway.frequency }
	};

	j["motion"]["vortex"] = {
		{ "useEmitterOffset", b.motion.vortex.useEmitterOffset },
		{ "center", ToJson(b.motion.vortex.center) },
		{ "axis", ToString(b.motion.vortex.axis) },
		{ "angularSpeedMin", b.motion.vortex.angularSpeedMin },
		{ "angularSpeedMax", b.motion.vortex.angularSpeedMax },
		{ "inwardSpeedMin", b.motion.vortex.inwardSpeedMin },
		{ "inwardSpeedMax", b.motion.vortex.inwardSpeedMax },
		{ "verticalSpeedMin", b.motion.vortex.verticalSpeedMin },
		{ "verticalSpeedMax", b.motion.vortex.verticalSpeedMax }
	};

	j["motion"]["pointField"] = {
		{ "enabled", b.motion.pointField.enabled },
		{ "useEmitterOffset", b.motion.pointField.useEmitterOffset },
		{ "center", ToJson(b.motion.pointField.center) },
		{ "radius", b.motion.pointField.radius },
		{ "attractionStrength", b.motion.pointField.attractionStrength },
		{ "repulsionStrength", b.motion.pointField.repulsionStrength },
		{ "orbitStrength", b.motion.pointField.orbitStrength },
		{ "orbitAxis", ToJson(b.motion.pointField.orbitAxis) },
		{ "falloff", b.motion.pointField.falloff },
		{ "damping", b.motion.pointField.damping }
	};

	j["motion"]["wind"] = {
		{ "enabled", b.motion.wind.enabled },
		{ "useEmitterOffset", b.motion.wind.useEmitterOffset },
		{ "center", ToJson(b.motion.wind.center) },
		{ "size", ToJson(b.motion.wind.size) },
		{ "direction", ToJson(b.motion.wind.direction) },
		{ "strength", b.motion.wind.strength },
		{ "smoothVelocity", b.motion.wind.smoothVelocity },
		{ "acceleration", b.motion.wind.acceleration },
		{ "recoverOutsideField", b.motion.wind.recoverOutsideField },
		{ "deceleration", b.motion.wind.deceleration },
		{ "enableBoundaryFalloff", b.motion.wind.enableBoundaryFalloff },
		{ "boundaryFalloff", b.motion.wind.boundaryFalloff },
		{ "turbulenceStrength", b.motion.wind.turbulenceStrength },
		{ "turbulenceFrequency", b.motion.wind.turbulenceFrequency },
		{ "turbulenceScale", b.motion.wind.turbulenceScale }
	};

	j["color"] = {
		{ "mode", ToString(b.color.mode) },
		{ "startColorMin", ToJson(b.color.startColorMin) },
		{ "startColorMax", ToJson(b.color.startColorMax) },
		{ "endColorMin", ToJson(b.color.endColorMin) },
		{ "endColorMax", ToJson(b.color.endColorMax) },
		{ "randomColorMin", ToJson(b.color.randomColorMin) },
		{ "randomColorMax", ToJson(b.color.randomColorMax) },
		{ "randomColorChangeIntervalMin", b.color.randomColorChangeIntervalMin },
		{ "randomColorChangeIntervalMax", b.color.randomColorChangeIntervalMax },
		{ "randomColorLerpSpeed", b.color.randomColorLerpSpeed }
	};

	WriteRender(j["render"], b.render);
}

void ReadBehavior(const json& j, ParticleManager::ParticleBehavior& b) {
	if (j.contains("life")) {
		const json& life = j.at("life");
		b.life.isLooping = life.value("isLooping", b.life.isLooping);
		b.life.loopDuration =
			(std::max)(life.value("loopDuration", b.life.loopDuration), 0.001f);
		b.life.loopPingPong =
			life.value("loopPingPong", b.life.loopPingPong);
		b.life.lifeTimeMin = life.value("lifeTimeMin", b.life.lifeTimeMin);
		b.life.lifeTimeMax = life.value("lifeTimeMax", b.life.lifeTimeMax);
		b.life.enableLifeFade = life.value("enableLifeFade", b.life.enableLifeFade);
		b.life.fadeOutStartRatio = life.value("fadeOutStartRatio", b.life.fadeOutStartRatio);
	}

	if (j.contains("scale")) {
		const json& scale = j.at("scale");
		if (scale.contains("startScaleMin")) b.scale.startScaleMin = ReadVector3(scale.at("startScaleMin"), b.scale.startScaleMin);
		if (scale.contains("startScaleMax")) b.scale.startScaleMax = ReadVector3(scale.at("startScaleMax"), b.scale.startScaleMax);
		b.scale.enableScaleOverLife = scale.value("enableScaleOverLife", b.scale.enableScaleOverLife);
		if (scale.contains("endScaleMin")) b.scale.endScaleMin = ReadVector3(scale.at("endScaleMin"), b.scale.endScaleMin);
		if (scale.contains("endScaleMax")) b.scale.endScaleMax = ReadVector3(scale.at("endScaleMax"), b.scale.endScaleMax);
	}

	if (j.contains("rotation")) {
		const json& rotation = j.at("rotation");
		if (rotation.contains("initialRotationMin")) {
			b.rotation.initialRotationMin =
				ReadVector3(rotation.at("initialRotationMin"), b.rotation.initialRotationMin);
		}
		if (rotation.contains("initialRotationMax")) {
			b.rotation.initialRotationMax =
				ReadVector3(rotation.at("initialRotationMax"), b.rotation.initialRotationMax);
		}
		b.rotation.enableRotationOverTime = rotation.value(
			"enableRotationOverTime",
			b.rotation.enableRotationOverTime
		);
		if (rotation.contains("rotationSpeed")) {
			b.rotation.rotationSpeed =
				ReadVector3(rotation.at("rotationSpeed"), b.rotation.rotationSpeed);
		}
		b.rotation.alignToVelocity = rotation.value(
			"alignToVelocity",
			b.rotation.alignToVelocity
		);
		b.rotation.alignAxis = ToParticleAlignmentAxis(
			rotation.value("alignAxis", ToString(b.rotation.alignAxis))
		);
	}

	if (j.contains("motion")) {
		const json& motion = j.at("motion");

		b.motion.mode = ToMovementMode(motion.value("mode", ToString(b.motion.mode)));

		if (motion.contains("linear")) {
			const json& linear = motion.at("linear");
			if (linear.contains("baseVelocity")) b.motion.linear.baseVelocity = ReadVector3(linear.at("baseVelocity"), b.motion.linear.baseVelocity);
			if (linear.contains("velocityRandomRange")) b.motion.linear.velocityRandomRange = ReadVector3(linear.at("velocityRandomRange"), b.motion.linear.velocityRandomRange);
			b.motion.linear.enableAcceleration = linear.value("enableAcceleration", b.motion.linear.enableAcceleration);
			if (linear.contains("baseAcceleration")) b.motion.linear.baseAcceleration = ReadVector3(linear.at("baseAcceleration"), b.motion.linear.baseAcceleration);
			if (linear.contains("accelerationRandomRange")) b.motion.linear.accelerationRandomRange = ReadVector3(linear.at("accelerationRandomRange"), b.motion.linear.accelerationRandomRange);
		}

		if (motion.contains("sway")) {
			const json& sway = motion.at("sway");
			b.motion.sway.amplitude = sway.value("amplitude", b.motion.sway.amplitude);
			b.motion.sway.frequency = sway.value("frequency", b.motion.sway.frequency);
		}

		if (motion.contains("vortex")) {
			const json& vortex = motion.at("vortex");
			b.motion.vortex.useEmitterOffset = vortex.value("useEmitterOffset", b.motion.vortex.useEmitterOffset);
			if (vortex.contains("center")) b.motion.vortex.center = ReadVector3(vortex.at("center"), b.motion.vortex.center);
			b.motion.vortex.axis = ToVortexAxis(vortex.value("axis", ToString(b.motion.vortex.axis)));
			b.motion.vortex.angularSpeedMin = vortex.value("angularSpeedMin", b.motion.vortex.angularSpeedMin);
			b.motion.vortex.angularSpeedMax = vortex.value("angularSpeedMax", b.motion.vortex.angularSpeedMax);
			b.motion.vortex.inwardSpeedMin = vortex.value("inwardSpeedMin", b.motion.vortex.inwardSpeedMin);
			b.motion.vortex.inwardSpeedMax = vortex.value("inwardSpeedMax", b.motion.vortex.inwardSpeedMax);
			b.motion.vortex.verticalSpeedMin = vortex.value("verticalSpeedMin", b.motion.vortex.verticalSpeedMin);
			b.motion.vortex.verticalSpeedMax = vortex.value("verticalSpeedMax", b.motion.vortex.verticalSpeedMax);
			CollapseMaxToMin(b.motion.vortex.angularSpeedMin, b.motion.vortex.angularSpeedMax);
			CollapseMaxToMin(b.motion.vortex.inwardSpeedMin, b.motion.vortex.inwardSpeedMax);
			CollapseMaxToMin(b.motion.vortex.verticalSpeedMin, b.motion.vortex.verticalSpeedMax);
		}

		if (motion.contains("pointField")) {
			const json& field = motion.at("pointField");
			b.motion.pointField.enabled = field.value("enabled", b.motion.pointField.enabled);
			b.motion.pointField.useEmitterOffset = field.value("useEmitterOffset", b.motion.pointField.useEmitterOffset);
			if (field.contains("center")) b.motion.pointField.center = ReadVector3(field.at("center"), b.motion.pointField.center);
			b.motion.pointField.radius = field.value("radius", b.motion.pointField.radius);
			b.motion.pointField.attractionStrength = field.value("attractionStrength", b.motion.pointField.attractionStrength);
			b.motion.pointField.repulsionStrength = field.value("repulsionStrength", b.motion.pointField.repulsionStrength);
			b.motion.pointField.orbitStrength = field.value("orbitStrength", b.motion.pointField.orbitStrength);
			if (field.contains("orbitAxis")) b.motion.pointField.orbitAxis = ReadVector3(field.at("orbitAxis"), b.motion.pointField.orbitAxis);
			b.motion.pointField.falloff = field.value("falloff", b.motion.pointField.falloff);
			b.motion.pointField.damping = field.value("damping", b.motion.pointField.damping);
		}

		if (motion.contains("wind")) {
			const json& wind = motion.at("wind");
			b.motion.wind.enabled = wind.value("enabled", b.motion.wind.enabled);
			b.motion.wind.useEmitterOffset =
				wind.value("useEmitterOffset", b.motion.wind.useEmitterOffset);
			if (wind.contains("center")) {
				b.motion.wind.center = ReadVector3(wind.at("center"), b.motion.wind.center);
			}
			if (wind.contains("size")) {
				b.motion.wind.size = ReadVector3(wind.at("size"), b.motion.wind.size);
			}
			if (wind.contains("direction")) {
				b.motion.wind.direction =
					ReadVector3(wind.at("direction"), b.motion.wind.direction);
			}
			b.motion.wind.strength = wind.value("strength", b.motion.wind.strength);
			b.motion.wind.smoothVelocity = wind.value("smoothVelocity", b.motion.wind.smoothVelocity);
			b.motion.wind.acceleration = wind.value("acceleration", b.motion.wind.acceleration);
			b.motion.wind.recoverOutsideField = wind.value("recoverOutsideField", b.motion.wind.recoverOutsideField);
			b.motion.wind.deceleration = wind.value("deceleration", b.motion.wind.deceleration);
			b.motion.wind.enableBoundaryFalloff = wind.value("enableBoundaryFalloff", b.motion.wind.enableBoundaryFalloff);
			b.motion.wind.boundaryFalloff = wind.value("boundaryFalloff", b.motion.wind.boundaryFalloff);
			b.motion.wind.turbulenceStrength =
				wind.value("turbulenceStrength", b.motion.wind.turbulenceStrength);
			b.motion.wind.turbulenceFrequency =
				wind.value("turbulenceFrequency", b.motion.wind.turbulenceFrequency);
			b.motion.wind.turbulenceScale =
				wind.value("turbulenceScale", b.motion.wind.turbulenceScale);
		}
	}

	if (j.contains("color")) {
		const json& color = j.at("color");

		b.color.mode = ToColorChangeMode(color.value("mode", ToString(b.color.mode)));
		if (color.contains("startColorMin")) b.color.startColorMin = ReadVector4(color.at("startColorMin"), b.color.startColorMin);
		if (color.contains("startColorMax")) b.color.startColorMax = ReadVector4(color.at("startColorMax"), b.color.startColorMax);
		if (color.contains("endColorMin")) b.color.endColorMin = ReadVector4(color.at("endColorMin"), b.color.endColorMin);
		if (color.contains("endColorMax")) b.color.endColorMax = ReadVector4(color.at("endColorMax"), b.color.endColorMax);
		if (color.contains("randomColorMin")) b.color.randomColorMin = ReadVector4(color.at("randomColorMin"), b.color.randomColorMin);
		if (color.contains("randomColorMax")) b.color.randomColorMax = ReadVector4(color.at("randomColorMax"), b.color.randomColorMax);

		b.color.randomColorChangeIntervalMin = color.value("randomColorChangeIntervalMin", b.color.randomColorChangeIntervalMin);
		b.color.randomColorChangeIntervalMax = color.value("randomColorChangeIntervalMax", b.color.randomColorChangeIntervalMax);
		b.color.randomColorLerpSpeed = color.value("randomColorLerpSpeed", b.color.randomColorLerpSpeed);
	}

	if (j.contains("render")) {
		ReadRender(j.at("render"), b.render);
	}
}

} // namespace

namespace ParticleEffectResource {

bool Save(const std::string& filePath, const ParticleEffectDesc& effect) {
	json root;

	root["name"] = effect.name;
	root["textureFilePath"] = effect.textureFilePath;
	root["simulationType"] = ToString(effect.simulationType);
	root["blendMode"] = ToString(effect.blendMode);

	root["emitter"] = {
		{ "translate", ToJson(effect.emitter.translate) },
		{ "spawnSize", ToJson(effect.emitter.spawnSize) },
		{ "count", effect.emitter.count },
		{ "frequency", effect.emitter.frequency },
		{ "isActive", effect.emitter.isActive }
	};

	root["lightning"] = {
		{ "enabled", effect.lightning.enabled },
		{ "startOffset", ToJson(effect.lightning.startOffset) },
		{ "endOffset", ToJson(effect.lightning.endOffset) },
		{ "randomRange", ToJson(effect.lightning.randomRange) },
		{ "coreColor", ToJson(effect.lightning.coreColor) },
		{ "branchColor", ToJson(effect.lightning.branchColor) },
		{ "jitter", effect.lightning.jitter },
		{ "branchLength", effect.lightning.branchLength },
		{ "branchProbability", effect.lightning.branchProbability },
		{ "thickness", effect.lightning.thickness },
		{ "duration", effect.lightning.duration },
		{ "segmentCount", effect.lightning.segmentCount },
		{ "flashExposure", effect.lightning.flashExposure },
		{ "flashExposureValue", effect.lightning.flashExposureValue },
		{ "flashReturnSpeed", effect.lightning.flashReturnSpeed }
	};

	WriteBehavior(root["behavior"], effect.behavior);

	std::ostringstream output;
	output << std::setw(2) << root << '\n';
	return EditableResourcePath::WriteTextAtomically(filePath, output.str());
}

bool Load(const std::string& filePath, ParticleEffectDesc& outEffect) {
	json root;
	const std::filesystem::path resolvedPath = EditableResourcePath::Resolve(filePath);
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
		} catch (...) {
		}
	}
	if (!parsed) {
		return false;
	}

	// Load into a fresh descriptor so missing optional fields do not inherit
	// values from the previously selected effect.
	ParticleEffectDesc loaded{};
	try {
	loaded.name = root.value("name", loaded.name);
	loaded.textureFilePath = root.value("textureFilePath", loaded.textureFilePath);
	loaded.simulationType = ToSimulationType(
		root.value("simulationType", "CPU")
	);
	loaded.blendMode = ToBlendMode(root.value("blendMode", ToString(loaded.blendMode)));

	if (root.contains("emitter")) {
		const json& emitter = root.at("emitter");

		if (emitter.contains("translate")) {
			loaded.emitter.translate = ReadVector3(emitter.at("translate"), loaded.emitter.translate);
		}
		if (emitter.contains("spawnSize")) {
			loaded.emitter.spawnSize = ReadVector3(emitter.at("spawnSize"), loaded.emitter.spawnSize);
		}

		loaded.emitter.count = emitter.value("count", loaded.emitter.count);
		loaded.emitter.frequency =
			NormalizeEmitterFrequency(emitter.value("frequency", loaded.emitter.frequency));
		loaded.emitter.isActive = emitter.value("isActive", loaded.emitter.isActive);
	}

	if (root.contains("lightning")) {
		const json& lightning = root.at("lightning");

		loaded.lightning.enabled =
			lightning.value("enabled", loaded.lightning.enabled);
		if (lightning.contains("startOffset")) {
			loaded.lightning.startOffset =
				ReadVector3(lightning.at("startOffset"), loaded.lightning.startOffset);
		}
		if (lightning.contains("endOffset")) {
			loaded.lightning.endOffset =
				ReadVector3(lightning.at("endOffset"), loaded.lightning.endOffset);
		}
		if (lightning.contains("randomRange")) {
			loaded.lightning.randomRange =
				ReadVector3(lightning.at("randomRange"), loaded.lightning.randomRange);
		}
		if (lightning.contains("coreColor")) {
			loaded.lightning.coreColor =
				ReadVector4(lightning.at("coreColor"), loaded.lightning.coreColor);
		}
		if (lightning.contains("branchColor")) {
			loaded.lightning.branchColor =
				ReadVector4(lightning.at("branchColor"), loaded.lightning.branchColor);
		}

		loaded.lightning.jitter =
			lightning.value("jitter", loaded.lightning.jitter);
		loaded.lightning.branchLength =
			lightning.value("branchLength", loaded.lightning.branchLength);
		loaded.lightning.branchProbability =
			lightning.value("branchProbability", loaded.lightning.branchProbability);
		loaded.lightning.thickness =
			lightning.value("thickness", loaded.lightning.thickness);
		loaded.lightning.duration =
			lightning.value("duration", loaded.lightning.duration);
		loaded.lightning.segmentCount =
			lightning.value("segmentCount", loaded.lightning.segmentCount);
		loaded.lightning.flashExposure =
			lightning.value("flashExposure", loaded.lightning.flashExposure);
		loaded.lightning.flashExposureValue =
			lightning.value("flashExposureValue", loaded.lightning.flashExposureValue);
		loaded.lightning.flashReturnSpeed =
			lightning.value("flashReturnSpeed", loaded.lightning.flashReturnSpeed);
	}

	if (root.contains("behavior")) {
		ReadBehavior(root.at("behavior"), loaded.behavior);
	}
	} catch (...) {
		return false;
	}

	outEffect = loaded;
	return true;
}

bool PrepareParticleGroup(const ParticleEffectDesc& effect, bool clearParticles) {
	if (effect.simulationType == ParticleSimulationType::kGPU) {
		(void)clearParticles;
		return ParticleManager::GetInstance()->ApplyGpuParticleEffect(effect);
	}

	ParticleManager* particleManager = ParticleManager::GetInstance();

	if (!particleManager->CreateParticleGroupIfNeeded(
		effect.name,
		effect.textureFilePath
	)) {
		return false;
	}
	if (!particleManager->SetParticleGroupTexture(
		effect.name,
		effect.textureFilePath
	)) {
		return false;
	}

	particleManager->SetGroupBlendMode(effect.name, effect.blendMode);
	particleManager->SetGroupRenderDesc(effect.name, effect.behavior.render);

	if (clearParticles) {
		particleManager->ClearParticleGroup(effect.name);
	}
	return true;
}

void ApplyToEmitter(ParticleEmitter& emitter, const ParticleEffectDesc& effect) {
	if (effect.simulationType == ParticleSimulationType::kGPU) {
		ParticleManager::GetInstance()->ApplyGpuParticleEffect(effect);
		return;
	}

	emitter.SetTranslate(effect.emitter.translate);
	emitter.SetSpawnSize(effect.emitter.spawnSize);
	emitter.SetCount(effect.emitter.count);
	emitter.SetFrequency(effect.emitter.frequency);
	emitter.SetActive(effect.emitter.isActive);
	emitter.SetBehavior(effect.behavior);
	emitter.SetLightning(effect.lightning);
}

ParticleEmitter* CreateEmitter(const ParticleEffectDesc& effect) {
	if (effect.simulationType == ParticleSimulationType::kGPU) {
		ParticleManager::GetInstance()->ApplyGpuParticleEffect(effect);
		return nullptr;
	}

	if (!PrepareParticleGroup(effect, true)) {
		return nullptr;
	}

	ParticleEmitter* emitter = new ParticleEmitter();
	emitter->Initialize(ParticleManager::GetInstance(), effect.name);
	ApplyToEmitter(*emitter, effect);

	return emitter;
}

} // namespace ParticleEffectResource
