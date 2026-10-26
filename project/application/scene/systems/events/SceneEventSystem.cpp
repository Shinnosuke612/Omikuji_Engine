// 役割: 汎用Trigger/Actionをゲーム進行要求へ変換する。
#include "SceneEventSystem.h"

#include "../gameplay/combat/SceneStatSystem.h"
#include "../gameplay/state/SceneStateMachineSystem.h"
#include "../../../../engine/io/Input.h"
#include "../../../../engine/math/Math.h"
#include "../../../../engine/scene/SceneDocument.h"
#include "../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../engine/scene/SceneInputKey.h"
#include "../../../../engine/scene/SceneTransformResolver.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <limits>

namespace {
	SceneEntity* ResolvePresentationOwner(SceneDocument& document,
		uint64_t id, const std::string& name) {
		if (id != 0) return document.FindEntity(id);
		if (name.empty()) return nullptr;
		SceneEntity* result = nullptr;
		for (SceneEntity& entity : document.GetEntities()) {
			if (entity.name != name) continue;
			if (result) return nullptr;
			result = &entity;
		}
		return result;
	}

	SceneEntity* ResolveEntity(
		SceneDocument& document,
		uint64_t id,
		const std::string& name,
		uint64_t fallbackId
	) {
		if (id != 0) {
			if (SceneEntity* entity = document.FindEntity(id)) {
				return entity;
			}
		}
		if (!name.empty()) {
			if (SceneEntity* entity = document.FindEntityByName(name)) {
				return entity;
			}
		}
		// 明示参照が壊れたActionをOwnerへ誤適用しない。
		return id == 0 && name.empty()
			? document.FindEntity(fallbackId)
			: nullptr;
	}

	bool CompareStat(float value, const SceneEventBinding& binding) {
		if (binding.statComparison == "GreaterOrEqual") {
			return value >= binding.statValue;
		}
		if (binding.statComparison == "Greater") {
			return value > binding.statValue;
		}
		if (binding.statComparison == "Less") {
			return value < binding.statValue;
		}
		if (binding.statComparison == "Equal") {
			return std::abs(value - binding.statValue) <= 0.0001f;
		}
		return value <= binding.statValue;
	}

	bool IsValidPostProcessProfileAction(
		const SceneDocument& document,
		const SceneEventAction& action
	) {
		const SceneEntity* manager = action.postProcessManagerEntityId != 0
			? document.FindEntity(action.postProcessManagerEntityId)
			: nullptr;
		if (!manager && !action.postProcessManagerEntityName.empty()) {
			manager = document.FindEntityByName(
				action.postProcessManagerEntityName
			);
		}
		if (!manager || action.postProcessProfileId.empty() ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(document, *manager)) {
			return false;
		}
		const SceneComponent* component =
			SceneEntityQuery::FindEnabledComponent(
				*manager,
				"PostProcessProfileManager"
			);
		if (!component) {
			return false;
		}
		return std::any_of(
			component->postProcessProfiles.begin(),
			component->postProcessProfiles.end(),
			[&action](const ScenePostProcessProfile& profile) {
				return profile.id == action.postProcessProfileId;
			}
		);
	}

	bool IsValidPostProcessManagerAction(
		const SceneDocument& document,
		const SceneEventAction& action
	) {
		const SceneEntity* manager = action.postProcessManagerEntityId != 0
			? document.FindEntity(action.postProcessManagerEntityId)
			: nullptr;
		if (!manager && !action.postProcessManagerEntityName.empty()) {
			manager = document.FindEntityByName(
				action.postProcessManagerEntityName
			);
		}
		const SceneComponent* component = manager &&
			SceneEntityQuery::IsEntityActiveInHierarchy(document, *manager)
			? SceneEntityQuery::FindEnabledComponent(
				*manager, "PostProcessProfileManager"
			)
			: nullptr;
		return component && !component->postProcessProfiles.empty();
	}

	bool IsValidCameraAction(
		const SceneDocument& document,
		const SceneEventAction& action,
		const char* componentName
	) {
		if (action.targetEntityId == 0 && action.targetEntityName.empty()) {
			return false;
		}
		const SceneEntity* entity = action.targetEntityId != 0
			? document.FindEntity(action.targetEntityId)
			: nullptr;
		if (!entity && !action.targetEntityName.empty()) {
			entity = document.FindEntityByName(action.targetEntityName);
		}
		return entity &&
			SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity) &&
			SceneEntityQuery::FindEnabledComponent(*entity, componentName);
	}

	bool IsValidAudioAction(const SceneDocument& document, const SceneEventAction& action) {
		const SceneEntity* entity = action.targetEntityId != 0
			? document.FindEntity(action.targetEntityId) : nullptr;
		if (!entity && !action.targetEntityName.empty()) {
			entity = document.FindEntityByName(action.targetEntityName);
		}
		return entity && SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity) &&
			SceneEntityQuery::FindEnabledComponent(*entity, "AudioSource");
	}

	bool IsValidTextMotionAction(
		const SceneDocument& document,
		const SceneEventAction& action
	) {
		const SceneEntity* entity = action.targetEntityId != 0
			? document.FindEntity(action.targetEntityId) : nullptr;
		if (!entity && !action.targetEntityName.empty()) {
			entity = document.FindEntityByName(action.targetEntityName);
		}
		const SceneComponent* component = entity &&
			SceneEntityQuery::IsEntityActiveInHierarchy(document, *entity)
			? SceneEntityQuery::FindEnabledComponent(*entity, "TextMotion")
			: nullptr;
		if (!component) {
			return false;
		}
		return action.type != "PlayTextMotion" || std::any_of(
			component->textMotionClips.begin(),
			component->textMotionClips.end(),
			[&action](const SceneTextMotionClip& clip) {
				return clip.id == action.textMotionClipId;
			}
		);
	}
}

SceneEventResult SceneEventSystem::Update(
	SceneDocument& document,
	SceneStatSystem& statSystem,
	SceneStateMachineSystem& stateMachineSystem,
	float deltaTime,
	const SceneEventRuntimeSignals& signals,
	const std::unordered_set<uint64_t>& blockedEntityIds,
	bool acceptGameplayInput
) {
	SceneEventResult result{};
	struct QueuedAction {
		uint64_t ownerEntityId = 0;
		SceneEventAction action{};
	};
	std::vector<QueuedAction> queuedActions;
	std::unordered_set<uint64_t> requiredEntities;

	for (const SceneEntity& entity : document.GetEntities()) {
		if (blockedEntityIds.contains(entity.id)) { requiredEntities.insert(entity.id); continue; }
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* eventComponent =
			SceneEntityQuery::FindEnabledComponent(entity, "EventTrigger");
		if (!eventComponent) {
			continue;
		}
		requiredEntities.insert(entity.id);
		auto& states = runtimes_[entity.id];
		states.resize(eventComponent->eventBindings.size());
		for (size_t index = 0;
			index < eventComponent->eventBindings.size();
			++index) {
			const SceneEventBinding& binding =
				eventComponent->eventBindings[index];
			// UI設定を通常Eventとして実行しない。UI制御段階は別の更新経路が担う。
			if (binding.executionDomain != "Gameplay") {
				continue;
			}
			BindingRuntime& state = states[index];
			state.cooldown = (std::max)(state.cooldown - deltaTime, 0.0f);
			SceneEntity* target = ResolveEntity(
				document,
				binding.targetEntityId,
				binding.targetEntityName,
				entity.id
			);
			bool condition = false;
			bool shouldFire = false;
			if (binding.triggerType == "OnStart") {
				shouldFire = !state.initialized;
				condition = true;
			} else if (binding.triggerType == "OnInterval") {
				condition = true;
				shouldFire = !state.initialized || state.cooldown <= 0.0f;
			} else if (binding.triggerType == "OnStatReachedMin") {
				condition = target && statSystem.IsAtMin(target->id, binding.statId);
				shouldFire = condition && !state.wasConditionTrue;
			} else if (binding.triggerType == "OnStatCompare") {
				float value = 0.0f;
				condition = target &&
					statSystem.TryGet(target->id, binding.statId, value) &&
					CompareStat(value, binding);
				shouldFire = condition && !state.wasConditionTrue;
			} else if (binding.triggerType == "OnPositionReached") {
				if (target) {
					const Transform transform =
						SceneTransformResolver::ResolveScene3DTransform(
							document,
							*target
						);
					condition = Math::Length(Math::Subtract(
						transform.translate,
						binding.targetPosition
					)) <= (std::max)(binding.radius, 0.0f);
				}
				shouldFire = condition && !state.wasConditionTrue;
			} else if (binding.triggerType == "OnKeyPressed") {
				if (!acceptGameplayInput) continue;
				Input* input = Input::GetInstance();
				const BYTE key = ResolveSceneInputKey(binding.triggerKey);
				condition = input && key != 0 && input->TriggerKey(key);
				shouldFire = condition;
			} else if (binding.triggerType == "OnCameraPathCompleted") {
				condition =
					(binding.targetEntityId != 0 ||
						!binding.targetEntityName.empty()) &&
					target &&
					target->id == signals.completedCameraPathEntityId;
				shouldFire = condition && !state.wasConditionTrue;
			} else if (binding.triggerType == "OnAudioFinished") {
				condition =
					(binding.targetEntityId != 0 ||
						!binding.targetEntityName.empty()) &&
					target &&
					SceneEntityQuery::FindEnabledComponent(*target, "AudioSource") &&
					std::find(
						signals.finishedAudioEntityIds.begin(),
						signals.finishedAudioEntityIds.end(),
						target->id
					) != signals.finishedAudioEntityIds.end();
				// 完了は状態ではなくgenerationごとのpulseなので、連続Frameも取りこぼさない。
				shouldFire = condition;
			} else if (binding.triggerType == "OnTextMotionCompleted") {
				condition = target &&
					SceneEntityQuery::FindEnabledComponent(*target, "TextMotion") &&
					std::any_of(
						signals.textMotionCompletions.begin(),
						signals.textMotionCompletions.end(),
						[&binding, target](const SceneTextMotionCompletion& completion) {
							return completion.entityId == target->id &&
								(binding.textMotionClipId.empty() ||
									completion.clipId == binding.textMotionClipId);
						}
					);
				shouldFire = condition;
			} else if (binding.triggerType == "OnPresentationClipCompleted") {
				const SceneEntity* presentationOwner = ResolvePresentationOwner(document,
					binding.targetEntityId, binding.targetEntityName);
				condition = presentationOwner && !binding.presentationClipName.empty() &&
					SceneEntityQuery::FindEnabledComponent(*presentationOwner, "PrefabAnimator") &&
					std::any_of(signals.presentationCompletions.begin(), signals.presentationCompletions.end(),
						[&](const ScenePresentationCompletion& completion) {
							return completion.entityId == presentationOwner->id &&
								completion.clipName == binding.presentationClipName;
						});
				// 自然終了pulseは連続Frameでもgenerationごとに評価する。
				shouldFire = condition;
			}

			if (
				shouldFire &&
				state.cooldown <= 0.0f &&
				(!binding.triggerOnce || !state.fired)
			) {
				for (const SceneEventAction& action : binding.actions) {
					queuedActions.push_back({ entity.id, action });
				}
				state.fired = true;
				state.cooldown = (std::max)(binding.cooldown, 0.0f);
			}
			state.wasConditionTrue = condition;
			state.initialized = true;
		}
	}

	for (auto iterator = runtimes_.begin(); iterator != runtimes_.end();) {
		if (!requiredEntities.contains(iterator->first)) {
			iterator = runtimes_.erase(iterator);
		} else {
			++iterator;
		}
	}

	for (const QueuedAction& queued : queuedActions) {
		const SceneEventAction& action = queued.action;
		SceneEntity* target = ResolveEntity(
			document,
			action.targetEntityId,
			action.targetEntityName,
			queued.ownerEntityId
		);
		// 外部Eventから出現中branchへ書かない。明示取消だけは次のSpawner検証へ渡す。
		if (target && blockedEntityIds.contains(target->id) && !(action.type == "SetEntityActive" && !action.active)) continue;
		if (action.type == "OpenMenu" || action.type == "PauseGame") {
			result.menuRequests.push_back({{}, queued.ownerEntityId, action});
		} else if (action.type == "ModifyStat") {
			if (target) {
				statSystem.Modify(
					target->id,
					action.statId,
					action.statOperation,
					action.value
				);
			}
		} else if (action.type == "SetEntityActive") {
			if (target) {
				target->active = action.active;
			}
		} else if (action.type == "InstantiatePrefab") {
			const uint64_t targetId = target ? target->id : 0;
			Transform spawnTransform{};
			const bool hasSpawnTransform =
				target && action.prefabUseTargetTransform;
			if (hasSpawnTransform) {
				spawnTransform = SceneTransformResolver::ResolveScene3DTransform(
					document,
					*target
				);
			}
			const uint64_t instanceId = document.InstantiatePrefab(
				action.prefabPath,
				action.prefabParentToTarget ? targetId : 0,
				true
			);
			if (
				instanceId != 0 &&
				hasSpawnTransform &&
				!action.prefabParentToTarget
			) {
				if (SceneEntity* instance = document.FindEntity(instanceId)) {
					instance->transform.translate = spawnTransform.translate;
					instance->transform.rotate = spawnTransform.quaternionRotate;
				}
			}
		} else if (action.type == "ChangeState") {
			if (target) {
				stateMachineSystem.RequestState(target->id, action.stateName);
			}
		} else if (
			action.type == "SceneTransition" &&
			result.sceneTransitionId.empty()
		) {
			result.sceneTransitionId = action.sceneId;
			result.transitionAssetPath = action.transitionAssetPath;
		} else if (
			action.type == "SetPostProcessProfile" &&
			IsValidPostProcessProfileAction(document, action)
		) {
			result.postProcessRequest.type =
				ScenePostProcessRequestType::SetProfile;
			result.postProcessRequest.managerEntityId =
				action.postProcessManagerEntityId;
			result.postProcessRequest.managerEntityName =
				action.postProcessManagerEntityName;
			result.postProcessRequest.profileId = action.postProcessProfileId;
		} else if (
			action.type == "NextPostProcessProfile" &&
			IsValidPostProcessManagerAction(document, action)
		) {
			result.postProcessRequest.type =
				ScenePostProcessRequestType::NextProfile;
			result.postProcessRequest.managerEntityId =
				action.postProcessManagerEntityId;
			result.postProcessRequest.managerEntityName =
				action.postProcessManagerEntityName;
			result.postProcessRequest.profileId.clear();
		} else if (action.type == "ResetPostProcessProfile") {
			result.postProcessRequest.type =
				ScenePostProcessRequestType::ResetToSceneDefault;
			result.postProcessRequest.managerEntityId = 0;
			result.postProcessRequest.managerEntityName.clear();
			result.postProcessRequest.profileId.clear();
		} else if (
			(action.type == "PlayAudio" || action.type == "StopAudio" ||
				action.type == "PauseAudio" || action.type == "ResumeAudio") &&
			IsValidAudioAction(document, action)
		) {
			SceneAudioRequestType type = SceneAudioRequestType::Play;
			if (action.type == "StopAudio") type = SceneAudioRequestType::Stop;
			else if (action.type == "PauseAudio") type = SceneAudioRequestType::Pause;
			else if (action.type == "ResumeAudio") type = SceneAudioRequestType::Resume;
			result.audioRequests.push_back({ type, target->id });
		} else if (
			(action.type == "PlayTextMotion" || action.type == "StopTextMotion" ||
				action.type == "ResetTextMotion") &&
			IsValidTextMotionAction(document, action)
		) {
			SceneTextMotionRequestType type = SceneTextMotionRequestType::Play;
			if (action.type == "StopTextMotion") type = SceneTextMotionRequestType::Stop;
			else if (action.type == "ResetTextMotion") type = SceneTextMotionRequestType::Reset;
			result.textMotionRequests.push_back({
				type,
				target->id,
				action.textMotionClipId
			});
		} else if (
			action.type == "PlayPresentationClip" || action.type == "StopPresentationClip" ||
				action.type == "ResetPresentationClip"
		) {
			const SceneEntity* presentationOwner = ResolvePresentationOwner(document,
				action.targetEntityId, action.targetEntityName);
			if (!presentationOwner) continue;
			ScenePresentationRequestType type = ScenePresentationRequestType::Play;
			if (action.type == "StopPresentationClip") type = ScenePresentationRequestType::Stop;
			else if (action.type == "ResetPresentationClip") type = ScenePresentationRequestType::Reset;
			// 非Active ownerも要求を受ける。Clipと親/所有競合は同期前に再検証する。
			result.presentationRequests.push_back({ type, presentationOwner->id, action.presentationClipName });
		} else if (
			action.type == "PlayCameraPath" &&
			IsValidCameraAction(document, action, "CameraPath")
		) {
			result.cameraRequests.push_back({
				SceneCameraRequestType::PlayPath,
				action.targetEntityId,
				action.targetEntityName
			});
		} else if (
			action.type == "StopCameraPath" &&
			IsValidCameraAction(document, action, "CameraPath")
		) {
			result.cameraRequests.push_back({
				SceneCameraRequestType::StopPath,
				action.targetEntityId,
				action.targetEntityName
			});
		} else if (
			action.type == "SelectCamera" &&
			IsValidCameraAction(document, action, "Camera")
		) {
			result.cameraRequests.push_back({
				SceneCameraRequestType::SelectCamera,
				action.targetEntityId,
				action.targetEntityName
			});
		}
	}
	if (!result.sceneTransitionId.empty()) {
		result.menuRequests.clear();
		result.postProcessRequest.type = ScenePostProcessRequestType::None;
		result.cameraRequests.clear();
		result.audioRequests.clear();
		result.textMotionRequests.clear();
		result.presentationRequests.clear();
	}
	return result;
}

void SceneEventSystem::Clear() {
	runtimes_.clear();
}

void SceneEventSystem::ResetEntity(uint64_t entityId) {
	runtimes_.erase(entityId);
}

void SceneEventSystem::ClearMenuInput() {
	menuRuntimes_.clear(); menuBindingSignatures_.clear(); menuKeyArmed_.fill(false);
	menuGeneration_ = menuInstance_ = 0;
	menuInputSignature_.clear();
}
namespace {
	bool UiOwnerActive(const SceneDocument& doc, const SceneEntity& owner) {
		const SceneEntity* current = &owner;
		for (size_t depth = 0; current && current->active && depth <= doc.GetEntities().size(); ++depth) {
			if (!current->parentId) return true;
			current = doc.FindEntity(current->parentId);
		}
		return false;
	}
	bool KnownUiAction(const std::string& type) {
		for (const char* name : {"OpenMenu", "CloseMenu", "SelectPreviousMenuItem", "SelectNextMenuItem", "ConfirmMenuItem", "CancelMenu",
			"SceneTransition", "PlayAudio", "StopAudio", "PauseAudio", "ResumeAudio", "PlayTextMotion", "StopTextMotion", "ResetTextMotion",
			"PauseGame", "ResumeGame", "RequestApplicationExit"}) if (type == name) return true;
		return false;
	}
	int UiPriority(const SceneEventBinding& binding) {
		int priority = 0;
		for (const auto& action : binding.actions) {
			if (action.type == "CancelMenu") priority = (std::max)(priority, 3);
			else if (action.type == "ConfirmMenuItem") priority = (std::max)(priority, 2);
			else if (action.type == "SelectPreviousMenuItem" || action.type == "SelectNextMenuItem") priority = (std::max)(priority, 1);
		}
		return priority;
	}
}
SceneEventSystem::BindingRuntime& SceneEventSystem::UiBindingState(uint64_t ownerId, size_t index, const SceneComponent& events) {
	auto& states = menuRuntimes_[ownerId]; states.resize(events.eventBindings.size());
	auto& signatures = menuBindingSignatures_[ownerId]; signatures.resize(events.eventBindings.size());
	const auto& b = events.eventBindings[index];
	std::ostringstream signature;
	signature << std::setprecision(std::numeric_limits<float>::max_digits10) << b.executionDomain << '|' << b.uiInputScope << '|' <<
		b.triggerType << '|' << b.triggerKey << '|' << b.targetEntityId << '|' << b.triggerOnce << '|' << b.cooldown;
	for (const auto& a : b.actions) signature << '|' << a.type << ':' << a.targetEntityId << ':' << a.targetEntityName << ':' << a.sceneId << ':' << a.transitionAssetPath << ':' << a.textMotionClipId;
	if (signatures[index] != signature.str()) { states[index] = {}; signatures[index] = signature.str(); }
	return states[index];
}
std::vector<SceneUiActionRequest> SceneEventSystem::EvaluateUiInput(const SceneDocument& document,
	const SceneMenuFrame& frame, float deltaTime, bool allowed) {
	std::vector<SceneUiActionRequest> result;
	if (frame.generation != document.GetContentGeneration() || frame.revision != document.GetRevision()) { RearmMenuKeys(); return result; }
	if (menuGeneration_ != frame.generation || menuInstance_ != frame.instanceId) ClearMenuInput();
	menuGeneration_ = frame.generation; menuInstance_ = frame.instanceId;
	const auto* panelOwner = document.FindEntity(frame.panelId);
	const auto* panel = panelOwner ? SceneEntityQuery::FindEnabledComponent(*panelOwner, "MenuPanel") : nullptr;
	const std::string inputSignature = std::to_string(frame.panelId) + ":" + std::to_string(frame.openSerial) +
		(panel ? panel->menuInputMode + ":" + panel->menuNavigationAxis : "");
	if (menuInputSignature_ != inputSignature) { RearmMenuKeys(); menuInputSignature_ = inputSignature; }
	if (!allowed) { RearmMenuKeys(); return result; }
	Input* input = Input::GetInstance();
	if (!input) return result;
	std::array<bool, 256> pressed{};
	for (size_t key = 1; key < menuKeyArmed_.size(); ++key) {
		const BYTE code = static_cast<BYTE>(key);
		if (!input->PushKey(code)) menuKeyArmed_[key] = true;
		pressed[key] = menuKeyArmed_[key] && input->TriggerKey(code);
	}
	struct Candidate { uint64_t owner; size_t index; const SceneComponent* events; int priority; bool standard = false; };
	std::vector<Candidate> candidates;
	std::unordered_set<BYTE> seenKeys;
	std::unordered_set<uint64_t> owners;
	int priority = -1;
	// 合成候補も同じ押下snapshotと優先順位を通す。Documentのbindingには保存しない。
	SceneComponent standardEvents("EventTrigger");
	if (panel && panel->menuInputMode == "Standard" && UiOwnerActive(document, *panelOwner)) {
		const bool horizontal = panel->menuNavigationAxis == "Horizontal";
		const char* keys[] = {horizontal ? "LEFT" : "UP", horizontal ? "RIGHT" : "DOWN", "ENTER", "ESCAPE"};
		const char* actions[] = {"SelectPreviousMenuItem", "SelectNextMenuItem", "ConfirmMenuItem", "CancelMenu"};
		for (size_t i = 0; i < 4; ++i) {
			SceneEventBinding binding; binding.triggerKey = keys[i]; binding.triggerOnce = false;
			SceneEventAction action; action.type = actions[i]; action.targetEntityId = frame.panelId;
			binding.actions.push_back(action); standardEvents.eventBindings.push_back(binding);
		}
		for (size_t i = 0; i < standardEvents.eventBindings.size(); ++i) {
			const auto& binding = standardEvents.eventBindings[i];
			const BYTE key = ResolveSceneInputKey(binding.triggerKey);
			seenKeys.insert(key); // 旧bindingに標準予約キーを横取りさせない。
			if (!pressed[key]) continue;
			const int rank = UiPriority(binding); priority = (std::max)(priority, rank);
			candidates.push_back({frame.panelId, i, &standardEvents, rank, true});
		}
	}
	const float elapsed = std::isfinite(deltaTime) ? (std::max)(deltaTime, 0.0f) : 0.0f;
	for (const auto& owner : document.GetEntities()) {
		if (!UiOwnerActive(document, owner)) continue;
		const auto* events = SceneEntityQuery::FindEnabledComponent(owner, "EventTrigger");
		if (!events) continue;
		owners.insert(owner.id);
		for (size_t i = 0; i < events->eventBindings.size(); ++i) {
			const auto& binding = events->eventBindings[i];
			if (binding.executionDomain != "UI" || binding.triggerType != "OnKeyPressed") continue;
			if (frame.panelId ? (binding.uiInputScope != "FocusedMenu" || binding.targetEntityId != frame.panelId) : binding.uiInputScope != "WhenNoMenu") continue;
			const bool supported = !binding.actions.empty() && std::all_of(binding.actions.begin(), binding.actions.end(), [&](const auto& action) {
				if (!KnownUiAction(action.type)) return false;
				const bool focused = action.type == "CloseMenu" || action.type == "SelectPreviousMenuItem" || action.type == "SelectNextMenuItem" || action.type == "ConfirmMenuItem" || action.type == "CancelMenu";
				return !focused || (frame.panelId && action.targetEntityId == frame.panelId);
			});
			const BYTE key = ResolveSceneInputKey(binding.triggerKey);
			if (!supported || !key || !std::isfinite(binding.cooldown) || binding.cooldown < 0 || !seenKeys.insert(key).second) continue;
			auto& state = UiBindingState(owner.id, i, *events);
			state.cooldown = (std::max)(0.0f, state.cooldown - elapsed);
			if (!pressed[key] || state.cooldown > 0 || (binding.triggerOnce && state.fired)) continue;
			const int candidatePriority = UiPriority(binding);
			priority = (std::max)(priority, candidatePriority);
			candidates.push_back({owner.id, i, events, candidatePriority});
		}
	}
	bool previous = false, next = false;
	for (const auto& c : candidates) if (c.priority == priority) for (const auto& a : c.events->eventBindings[c.index].actions) {
		previous |= a.type == "SelectPreviousMenuItem"; next |= a.type == "SelectNextMenuItem";
	}
	if (!(priority == 1 && previous && next)) for (const auto& c : candidates) {
		if (c.priority != priority) continue;
		const auto& binding = c.events->eventBindings[c.index];
		if (!c.standard) {
			auto& state = UiBindingState(c.owner, c.index, *c.events);
			state.fired = true; state.cooldown = binding.cooldown;
		}
		for (const auto& action : binding.actions) {
			const int actionPriority = action.type == "CancelMenu" ? 3 : action.type == "ConfirmMenuItem" ? 2 :
				(action.type == "SelectPreviousMenuItem" || action.type == "SelectNextMenuItem") ? 1 : 0;
			if (actionPriority && actionPriority < priority) continue;
			result.push_back({{frame.instanceId, frame.generation, frame.revision, frame.panelId, frame.openSerial}, c.owner, action,
				c.standard ? SceneUiActionRequest::Origin::PanelStandard : SceneUiActionRequest::Origin::EventBinding});
		}
		// 決定/キャンセルは一Frame一操作。複数の同義キーでもpulseを重複させない。
		if (priority >= 2) break;
	}
	for (auto it = menuRuntimes_.begin(); it != menuRuntimes_.end();) {
		if (!owners.contains(it->first)) { menuBindingSignatures_.erase(it->first); it = menuRuntimes_.erase(it); } else ++it;
	}
	return result;
}
std::vector<SceneUiActionRequest> SceneEventSystem::EvaluateUiNotifications(const SceneDocument& document,
	const std::vector<SceneMenuPulse>& pulses, float deltaTime) {
	std::vector<SceneUiActionRequest> result;
	const float elapsed = std::isfinite(deltaTime) ? (std::max)(deltaTime, 0.0f) : 0.0f;
	// pulseがないFrameもclockを更新する。実際の通知処理はdelta=0で呼べる。
	if (elapsed > 0) for (const auto& owner : document.GetEntities()) {
		if (!UiOwnerActive(document, owner)) continue;
		const auto* events = SceneEntityQuery::FindEnabledComponent(owner, "EventTrigger");
		if (!events) continue;
		for (size_t i = 0; i < events->eventBindings.size(); ++i) {
			const auto& binding = events->eventBindings[i];
			if (binding.executionDomain != "UI" || (binding.triggerType != "OnMenuItemConfirmed" && binding.triggerType != "OnMenuCancelled")) continue;
			auto& state = UiBindingState(owner.id, i, *events);
			state.cooldown = (std::max)(0.0f, state.cooldown - elapsed);
		}
	}
	// Cancelled→Confirmed等、MenuSystemが生成した通知順をownerのHierarchy順へ並べ替えない。
	for (const auto& pulse : pulses) {
		if (pulse.source.generation != document.GetContentGeneration() || pulse.source.revision != document.GetRevision()) continue;
		for (const auto& owner : document.GetEntities()) {
			if (!UiOwnerActive(document, owner)) continue;
			const auto* events = SceneEntityQuery::FindEnabledComponent(owner, "EventTrigger");
			if (!events) continue;
			for (size_t i = 0; i < events->eventBindings.size(); ++i) {
				const auto& binding = events->eventBindings[i];
				if (binding.executionDomain != "UI" || !binding.targetEntityId || !std::isfinite(binding.cooldown) || binding.cooldown < 0) continue;
				const bool matches = binding.triggerType == "OnMenuItemConfirmed" ? pulse.type == SceneMenuPulseType::ItemConfirmed && pulse.itemId == binding.targetEntityId :
					binding.triggerType == "OnMenuCancelled" && pulse.type == SceneMenuPulseType::Cancelled && pulse.source.panelId == binding.targetEntityId;
				if (!matches) continue;
				auto& state = UiBindingState(owner.id, i, *events);
				if (state.cooldown > 0 || (binding.triggerOnce && state.fired)) continue;
				state.fired = true; state.cooldown = binding.cooldown;
				for (const auto& action : binding.actions) result.push_back({pulse.source, owner.id, action});
			}
		}
	}
	return result;
}
SceneMenuMove SceneEventSystem::EvaluateMenuInput(const SceneDocument& document,
	const SceneMenuFrame& frame, float deltaTime, bool allowed) {
	SceneMenuMove result{frame.instanceId, frame.generation, frame.revision, frame.panelId};
	for (const auto& request : EvaluateUiInput(document, frame, deltaTime, allowed)) {
		result.previous |= request.action.type == "SelectPreviousMenuItem";
		result.next |= request.action.type == "SelectNextMenuItem";
	}
	return result;
}
