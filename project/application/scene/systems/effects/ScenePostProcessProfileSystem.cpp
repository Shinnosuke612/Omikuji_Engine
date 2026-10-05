// 役割: Post Process Profileの実行時選択をDocumentへ書き戻さずに管理する。
#include "ScenePostProcessProfileSystem.h"

#include "../../../../engine/scene/SceneDocument.h"
#include "../../../../engine/scene/SceneEntityQuery.h"

#include <algorithm>
#include <iterator>

namespace {
	const SceneEntity* ResolveManager(
		const SceneDocument& document,
		uint64_t entityId,
		const std::string& entityName
	) {
		const SceneEntity* entity = entityId != 0
			? document.FindEntity(entityId)
			: nullptr;
		if (!entity && !entityName.empty()) {
			entity = document.FindEntityByName(entityName);
		}
		if (!entity ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity) ||
			!SceneEntityQuery::FindEnabledComponent(*entity, "PostProcessProfileManager")) {
			return nullptr;
		}
		return entity;
	}
}

void ScenePostProcessProfileSystem::ApplyStatusBinding(
	const SceneComponent* component
) {
	statusTextEntityId_ = component
		? component->postProcessStatusTextEntityId
		: 0;
	statusTextEntityName_ = component
		? component->postProcessStatusTextEntityName
		: std::string{};
	statusTextPrefix_ = component
		? component->postProcessStatusTextPrefix
		: "PostEffect: ";
}

void ScenePostProcessProfileSystem::ClearAutomation() {
	automationActive_ = false;
	automationStartValue_ = 0.0f;
	automationEndValue_ = 1.0f;
	automationDuration_ = 1.0f;
	automationElapsed_ = 0.0f;
}

void ScenePostProcessProfileSystem::ApplyProfile(
	const SceneEntity& manager,
	const SceneComponent& component,
	const ScenePostProcessProfile& profile
) {
	effectiveSettings_ = profile.settings;
	activeManagerEntityId_ = manager.id;
	activeProfileId_ = profile.id;
	activeProfileLabel_ = profile.label;
	ApplyStatusBinding(&component);
	ClearAutomation();
	if (!profile.automations.empty()) {
		const ScenePostProcessAutomation& automation =
			profile.automations.front();
		automationActive_ = true;
		automationStartValue_ = automation.startValue;
		automationEndValue_ = automation.endValue;
		automationDuration_ = (std::max)(automation.duration, 0.001f);
		effectiveSettings_.dissolveThreshold = automationStartValue_;
	}
	++generation_;
}

void ScenePostProcessProfileSystem::ApplyBaseline(const SceneDocument& document) {
	effectiveSettings_ = document.GetPostProcessSettings();
	activeManagerEntityId_ = 0;
	activeProfileId_.clear();
	activeProfileLabel_ = "None";
	ClearAutomation();
	ApplyStatusBinding(nullptr);
	for (const SceneEntity& entity : document.GetEntities()) {
		const SceneComponent* component =
			SceneEntityQuery::FindEnabledComponent(
				entity, "PostProcessProfileManager"
			);
		if (component &&
			(component->postProcessStatusTextEntityId != 0 ||
				!component->postProcessStatusTextEntityName.empty())) {
			ApplyStatusBinding(component);
			break;
		}
	}
	++generation_;
}

void ScenePostProcessProfileSystem::Reset(const SceneDocument* document) {
	if (document) {
		ApplyBaseline(*document);
		return;
	}
	effectiveSettings_ = {};
	activeManagerEntityId_ = 0;
	activeProfileId_.clear();
	activeProfileLabel_ = "None";
	ClearAutomation();
	ApplyStatusBinding(nullptr);
	++generation_;
}

void ScenePostProcessProfileSystem::Sync(const SceneDocument& document) {
	if (activeManagerEntityId_ == 0) {
		ApplyStatusBinding(nullptr);
		for (const SceneEntity& entity : document.GetEntities()) {
			const SceneComponent* component =
				SceneEntityQuery::FindEnabledComponent(
					entity, "PostProcessProfileManager"
				);
			if (component &&
				(component->postProcessStatusTextEntityId != 0 ||
					!component->postProcessStatusTextEntityName.empty())) {
				ApplyStatusBinding(component);
				break;
			}
		}
		return;
	}
	const SceneEntity* manager = ResolveManager(
		document, activeManagerEntityId_, {}
	);
	if (!manager) {
		ApplyBaseline(document);
		return;
	}
	const SceneComponent* component = SceneEntityQuery::FindEnabledComponent(
		*manager, "PostProcessProfileManager"
	);
	ApplyStatusBinding(component);
	const auto profile = std::find_if(
		component->postProcessProfiles.begin(),
		component->postProcessProfiles.end(),
		[this](const ScenePostProcessProfile& candidate) {
			return candidate.id == activeProfileId_;
		}
	);
	if (profile == component->postProcessProfiles.end()) {
		ApplyBaseline(document);
	}
}

void ScenePostProcessProfileSystem::ApplyEventResult(
	const SceneDocument& document,
	const SceneEventResult& result
) {
	const ScenePostProcessRequest& request = result.postProcessRequest;
	if (request.type == ScenePostProcessRequestType::None) {
		return;
	}
	if (request.type == ScenePostProcessRequestType::ResetToSceneDefault) {
		ApplyBaseline(document);
		return;
	}
	const SceneEntity* manager = ResolveManager(
		document, request.managerEntityId, request.managerEntityName
	);
	if (!manager) {
		return;
	}
	const SceneComponent* component = SceneEntityQuery::FindEnabledComponent(
		*manager, "PostProcessProfileManager"
	);
	if (request.type == ScenePostProcessRequestType::NextProfile) {
		if (component->postProcessProfiles.empty()) {
			return;
		}
		const bool continuesCurrentManager =
			activeManagerEntityId_ == manager->id;
		auto current = continuesCurrentManager
			? std::find_if(
				component->postProcessProfiles.begin(),
				component->postProcessProfiles.end(),
				[this](const ScenePostProcessProfile& candidate) {
					return candidate.id == activeProfileId_;
				}
			)
			: component->postProcessProfiles.end();
		const auto next = current == component->postProcessProfiles.end() ||
			std::next(current) == component->postProcessProfiles.end()
			? component->postProcessProfiles.begin()
			: std::next(current);
		ApplyProfile(*manager, *component, *next);
		return;
	}
	if (
		request.type != ScenePostProcessRequestType::SetProfile ||
		request.profileId.empty()
	) {
		return;
	}
	const auto profile = std::find_if(
		component->postProcessProfiles.begin(),
		component->postProcessProfiles.end(),
		[&request](const ScenePostProcessProfile& candidate) {
			return candidate.id == request.profileId;
		}
	);
	if (profile == component->postProcessProfiles.end()) {
		return;
	}
	ApplyProfile(*manager, *component, *profile);
}

void ScenePostProcessProfileSystem::Update(float deltaTime) {
	if (!automationActive_ || deltaTime <= 0.0f) {
		return;
	}
	automationElapsed_ = (std::min)(
		automationElapsed_ + deltaTime,
		automationDuration_
	);
	const float progress = automationElapsed_ / automationDuration_;
	effectiveSettings_.dissolveThreshold =
		automationStartValue_ +
		(automationEndValue_ - automationStartValue_) * progress;
	if (automationElapsed_ >= automationDuration_) {
		automationActive_ = false;
	}
	++generation_;
}
