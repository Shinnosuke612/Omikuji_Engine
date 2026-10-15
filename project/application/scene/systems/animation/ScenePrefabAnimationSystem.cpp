// 役割: Gameplay Clip/Cross Fadeと、復元可能なTransform演出Clipを共通Evaluatorで再生する。
#include "ScenePrefabAnimationSystem.h"

#include "../../../../engine/scene/SceneDocument.h"
#include "../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../engine/scene/ScenePrefabAnimationEvaluator.h"
#include "../../../../engine/math/Math.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace {
	uint8_t TransformPropertyMask(const std::string& property) {
		if (property == "LocalPosition") return 0x1u;
		if (property == "LocalRotation") return 0x2u;
		if (property == "LocalScale") return 0x4u;
		return 0;
	}

	const SceneEntity* ResolvePresentationTarget(const SceneDocument& document,
		uint64_t ownerId, const SceneAnimationTrack& track) {
		// 明示IDが壊れたとき、同名Entityやownerへ付け替えない。
		if (track.targetEntityId != 0) return document.FindEntity(track.targetEntityId);
		if (track.targetEntityName.empty()) return document.FindEntity(ownerId);
		const SceneEntity* result = nullptr;
		for (const SceneEntity& entity : document.GetEntities()) {
			if (entity.name != track.targetEntityName) continue;
			if (result) return nullptr;
			result = &entity;
		}
		return result;
	}

	bool HasPresentationConflict(const SceneEntity& entity) {
		for (const char* type : { "PhysicsBody", "PlayerBehavior", "AttackSet",
			"StateMachine", "EnemyBehavior", "AgentBehavior", "Projectile", "BoneAttachment" }) {
			if (SceneEntityQuery::FindEnabledComponent(entity, type)) return true;
		}
		return false;
	}

	bool HasActiveParents(const SceneDocument& document, const SceneEntity& entity) {
		std::unordered_set<uint64_t> visited{ entity.id };
		uint64_t parentId = entity.parentId;
		while (parentId != 0) {
			const SceneEntity* parent = document.FindEntity(parentId);
			if (!parent || !parent->active || !visited.insert(parentId).second) return false;
			parentId = parent->parentId;
		}
		return true;
	}

	bool IsActiveBelowPresentationOwner(const SceneDocument& document,
		const SceneEntity& target, uint64_t ownerId) {
		const SceneEntity* current = &target;
		std::unordered_set<uint64_t> visited;
		while (current && current->id != ownerId) {
			if (!current->active || !visited.insert(current->id).second) return false;
			current = document.FindEntity(current->parentId);
		}
		return current != nullptr;
	}

	const ScenePrefabAnimationClip* FindPresentationClip(const SceneDocument& document,
		uint64_t ownerId, const std::string& name) {
		const SceneEntity* owner = document.FindEntity(ownerId);
		const SceneComponent* animator = owner
			? SceneEntityQuery::FindEnabledComponent(*owner, "PrefabAnimator") : nullptr;
		if (!owner || !animator || name.empty() || HasPresentationConflict(*owner) ||
			!HasActiveParents(document, *owner)) return nullptr;
		const ScenePrefabAnimationClip* result = nullptr;
		for (const ScenePrefabAnimationClip& clip : animator->prefabAnimationClips) {
			// 他Clipの自動再生も同じAnimatorの所有競合になる。
			if (clip.playOnStart) return nullptr;
			if (clip.name != name) continue;
			if (result) return nullptr;
			result = &clip;
		}
		if (!result || result->loop || !std::isfinite(result->duration) ||
			result->duration <= 0.0f) return nullptr;
		std::unordered_map<uint64_t, uint8_t> properties;
		for (const SceneAnimationTrack& track : result->tracks) {
			const uint8_t mask = TransformPropertyMask(track.property);
			const SceneEntity* target = ResolvePresentationTarget(document, ownerId, track);
			if (!mask || !target || HasPresentationConflict(*target) ||
				(target->id != ownerId && !document.IsDescendantOf(target->id, ownerId)) ||
				!IsActiveBelowPresentationOwner(document, *target, ownerId) ||
				(properties[target->id] & mask) != 0) return nullptr;
			properties[target->id] |= mask;
			for (const SceneAnimationKeyframe& key : track.keyframes) {
				if (!std::isfinite(key.time) || !std::isfinite(key.value.x) ||
					!std::isfinite(key.value.y) || !std::isfinite(key.value.z) ||
					!std::isfinite(key.positionBulge.x) || !std::isfinite(key.positionBulge.y) ||
					!std::isfinite(key.positionBulge.z)) return nullptr;
			}
		}
		return result;
	}

	const SceneEntity* ResolveTrackTarget(
		const SceneDocument& document,
		uint64_t ownerEntityId,
		const SceneAnimationTrack& track
	) {
		if (track.targetEntityId != 0) {
			if (const SceneEntity* entity =
				document.FindEntity(track.targetEntityId)) {
				return entity;
			}
		}
		if (!track.targetEntityName.empty()) {
			if (const SceneEntity* entity =
				document.FindEntityByName(track.targetEntityName)) {
				return entity;
			}
		}
		return document.FindEntity(ownerEntityId);
	}

	Vector3 LerpVector(
		const Vector3& start,
		const Vector3& destination,
		float amount
	) {
		return {
			Math::Lerp(start.x, destination.x, amount),
			Math::Lerp(start.y, destination.y, amount),
			Math::Lerp(start.z, destination.z, amount)
		};
	}
}

void ScenePrefabAnimationSystem::Update(
	SceneDocument& document,
	float deltaTime
) {
	std::unordered_set<uint64_t> requiredEntities;
	for (SceneEntity& entity : document.GetEntities()) {
		// 演出用ownerは専用更新でsampleする。Gameplay Clipの時刻/Loop規則は変えない。
		if (presentationRuntimes_.contains(entity.id)) {
			// 後段のGameplay Play要求を失わず、次回の所有競合検出へ残す。
			requiredEntities.insert(entity.id);
			continue;
		}
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* animator =
			SceneEntityQuery::FindEnabledComponent(entity, "PrefabAnimator");
		if (!animator || animator->prefabAnimationClips.empty()) {
			continue;
		}
		requiredEntities.insert(entity.id);
		AnimationRuntime& runtime = runtimes_[entity.id];
		if (!runtime.initialized) {
			runtime.initialized = true;
			for (size_t index = 0;
				index < animator->prefabAnimationClips.size();
				++index) {
				if (animator->prefabAnimationClips[index].playOnStart) {
					runtime.clipIndex = index;
					runtime.playing = true;
					break;
				}
			}
		}
		if (runtime.clipIndex >= animator->prefabAnimationClips.size()) {
			continue;
		}
		if (!runtime.playing && runtime.transitionPoses.empty()) {
			continue;
		}
		const ScenePrefabAnimationClip& clip =
			animator->prefabAnimationClips[runtime.clipIndex];
		const float safeDeltaTime = (std::max)(deltaTime, 0.0f);
		if (runtime.playing) {
			runtime.time += safeDeltaTime;
		}
		const float duration = (std::max)(clip.duration, 0.001f);
		if (runtime.playing && clip.loop) {
			runtime.time = std::fmod(runtime.time, duration);
		} else if (runtime.playing && runtime.time >= duration) {
			runtime.time = duration;
			runtime.playing = false;
		}

		ScenePrefabAnimationEvaluator::ApplyClip(
			document,
			entity.id,
			clip,
			runtime.time
		);

		if (!runtime.transitionPoses.empty()) {
			runtime.transitionTime += safeDeltaTime;
			const float transitionAmount = Math::SmoothStep(std::clamp(
				runtime.transitionTime /
					(std::max)(runtime.transitionDuration, 0.0001f),
				0.0f,
				1.0f
			));
			for (const TransitionPose& startPose : runtime.transitionPoses) {
				SceneEntity* target = document.FindEntity(startPose.entityId);
				if (!target) {
					continue;
				}
				if ((startPose.propertyMask & 0x1u) != 0) {
					target->transform.translate = LerpVector(
						startPose.transform.translate,
						target->transform.translate,
						transitionAmount
					);
				}
				if ((startPose.propertyMask & 0x2u) != 0) {
					target->transform.rotate = Slerp(
						startPose.transform.rotate,
						target->transform.rotate,
						transitionAmount
					);
				}
				if ((startPose.propertyMask & 0x4u) != 0) {
					target->transform.scale = LerpVector(
						startPose.transform.scale,
						target->transform.scale,
						transitionAmount
					);
				}
			}
			if (transitionAmount >= 1.0f) {
				runtime.transitionPoses.clear();
				runtime.transitionTime = 0.0f;
				runtime.transitionDuration = 0.0f;
			}
		}
	}

	for (auto iterator = runtimes_.begin(); iterator != runtimes_.end();) {
		if (!requiredEntities.contains(iterator->first)) {
			iterator = runtimes_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

bool ScenePrefabAnimationSystem::Play(
	const SceneDocument& document,
	uint64_t entityId,
	const std::string& clipName,
	bool restart,
	float transitionDuration
) {
	const SceneEntity* entity = document.FindEntity(entityId);
	const SceneComponent* animator = entity
		? SceneEntityQuery::FindEnabledComponent(*entity, "PrefabAnimator")
		: nullptr;
	if (!animator) {
		return false;
	}
	for (size_t index = 0; index < animator->prefabAnimationClips.size(); ++index) {
		if (animator->prefabAnimationClips[index].name != clipName) {
			continue;
		}
		AnimationRuntime& runtime = runtimes_[entityId];
		runtime.transitionPoses.clear();
		runtime.transitionTime = 0.0f;
		runtime.transitionDuration = (std::max)(transitionDuration, 0.0f);
		if (runtime.transitionDuration > 0.0f) {
			const ScenePrefabAnimationClip& destinationClip =
				animator->prefabAnimationClips[index];
			for (const SceneAnimationTrack& track : destinationClip.tracks) {
				uint8_t propertyMask = 0;
				if (track.property == "LocalPosition") {
					propertyMask = 0x1u;
				} else if (track.property == "LocalRotation") {
					propertyMask = 0x2u;
				} else if (track.property == "LocalScale") {
					propertyMask = 0x4u;
				} else {
					continue;
				}
				const SceneEntity* target = ResolveTrackTarget(
					document,
					entityId,
					track
				);
				if (!target) {
					continue;
				}
				auto found = std::find_if(
					runtime.transitionPoses.begin(),
					runtime.transitionPoses.end(),
					[target](const TransitionPose& pose) {
						return pose.entityId == target->id;
					}
				);
				if (found == runtime.transitionPoses.end()) {
					runtime.transitionPoses.push_back({
						target->id,
						target->transform,
						propertyMask
					});
				} else {
					found->propertyMask |= propertyMask;
				}
			}
		}
		if (restart || runtime.clipIndex != index) {
			runtime.time = 0.0f;
		}
		runtime.clipIndex = index;
		runtime.initialized = true;
		runtime.playing = true;
		return true;
	}
	return false;
}

void ScenePrefabAnimationSystem::ResetEntity(uint64_t entityId) {
	runtimes_.erase(entityId);
}

bool ScenePrefabAnimationSystem::HasGameplayOwnership(const SceneDocument& document,
	const std::vector<TransitionPose>& targets, uint64_t activatingOwnerId) const {
	for (const SceneEntity& owner : document.GetEntities()) {
		const SceneComponent* animator = SceneEntityQuery::FindEnabledComponent(owner, "PrefabAnimator");
		if (!animator) continue;
		const ScenePrefabAnimationClip* clip = nullptr;
		const auto found = runtimes_.find(owner.id);
		if (found != runtimes_.end() && found->second.initialized) {
			const AnimationRuntime& runtime = found->second;
			if ((runtime.playing || !runtime.transitionPoses.empty()) &&
				runtime.clipIndex < animator->prefabAnimationClips.size()) {
				clip = &animator->prefabAnimationClips[runtime.clipIndex];
			}
		} else if (SceneEntityQuery::IsEntityActiveInHierarchy(document, owner) ||
			(activatingOwnerId != 0 && IsActiveBelowPresentationOwner(document, owner, activatingOwnerId))) {
			// まだUpdateされていない別Animatorの自動再生も、同期後の上書きを起こす。
			const auto automatic = std::find_if(animator->prefabAnimationClips.begin(),
				animator->prefabAnimationClips.end(), [](const ScenePrefabAnimationClip& value) { return value.playOnStart; });
			if (automatic != animator->prefabAnimationClips.end()) clip = &*automatic;
		}
		if (!clip) continue;
		for (const SceneAnimationTrack& track : clip->tracks) {
			const SceneEntity* target = ResolveTrackTarget(document, owner.id, track);
			if (target && std::any_of(targets.begin(), targets.end(),
				[&](const TransitionPose& pose) {
					return pose.entityId == target->id &&
						(pose.propertyMask & TransformPropertyMask(track.property)) != 0;
				})) return true;
		}
	}
	return false;
}

bool ScenePrefabAnimationSystem::PlayPresentation(SceneDocument& document,
	uint64_t entityId, const std::string& clipName) {
	if (!BindPresentationDocument(document)) return false;
	const ScenePrefabAnimationClip* clip = FindPresentationClip(document, entityId, clipName);
	if (!clip) return false;
	std::vector<TransitionPose> targets;
	for (const SceneAnimationTrack& track : clip->tracks) {
		if (track.keyframes.empty()) continue;
		const SceneEntity* target = ResolvePresentationTarget(document, entityId, track);
		targets.push_back({ target->id, target->transform, TransformPropertyMask(track.property) });
	}
	if (HasGameplayOwnership(document, targets, entityId)) return false;
	for (const auto& [otherId, other] : presentationRuntimes_) {
		if (otherId == entityId) continue;
		for (const TransitionPose& pose : other.originalPoses) {
			if (std::any_of(targets.begin(), targets.end(), [&](const TransitionPose& target) {
				return pose.entityId == target.entityId && (pose.propertyMask & target.propertyMask) != 0;
			})) return false;
		}
	}
	SceneEntity* owner = document.FindEntity(entityId);
	presentationOriginalActive_.try_emplace(entityId, owner->active);
	PresentationRuntime& runtime = presentationRuntimes_[entityId];
	// 再PlayでTrackが増えた場合も、既に所有したpropertyの退避値を上書きしない。
	for (const TransitionPose& target : targets) {
		auto original = std::find_if(runtime.originalPoses.begin(), runtime.originalPoses.end(),
			[&](const TransitionPose& pose) { return pose.entityId == target.entityId; });
		if (original == runtime.originalPoses.end()) {
			runtime.originalPoses.push_back(target);
		} else {
			const uint8_t added = static_cast<uint8_t>(target.propertyMask & ~original->propertyMask);
			if (added & 0x1u) original->transform.translate = target.transform.translate;
			if (added & 0x2u) original->transform.rotate = target.transform.rotate;
			if (added & 0x4u) original->transform.scale = target.transform.scale;
			original->propertyMask |= target.propertyMask;
		}
	}
	runtime.clipTargets = std::move(targets);
	runtime.clipName = clipName;
	runtime.time = 0.0f;
	runtime.generation = ++presentationGeneration_;
	runtime.playing = true;
	runtime.justStarted = true;
	RemovePresentationCompletions(entityId);
	owner->active = true;
	ScenePrefabAnimationEvaluator::ApplyClip(document, entityId, *clip, 0.0f);
	return true;
}

void ScenePrefabAnimationSystem::DiscardPresentations() {
	presentationRuntimes_.clear();
	presentationOriginalActive_.clear();
	presentationCompletions_.clear();
	presentationDocumentGeneration_ = 0;
}

bool ScenePrefabAnimationSystem::BindPresentationDocument(const SceneDocument& document) {
	const uint64_t generation = document.GetContentGeneration();
	if (presentationDocumentGeneration_ != generation) {
		// 同じpointer/Entity IDでも内容差替え後は別Scene。Entity参照より先に破棄する。
		DiscardPresentations();
		presentationDocumentGeneration_ = generation;
	}
	return generation != 0;
}

void ScenePrefabAnimationSystem::RemovePresentationCompletions(uint64_t entityId) {
	std::erase_if(presentationCompletions_, [entityId](const ScenePresentationCompletion& completion) {
		return completion.entityId == entityId;
	});
}

void ScenePrefabAnimationSystem::StopPresentation(uint64_t entityId) {
	const auto found = presentationRuntimes_.find(entityId);
	if (found != presentationRuntimes_.end()) found->second.playing = false;
	RemovePresentationCompletions(entityId);
}

void ScenePrefabAnimationSystem::RestorePresentation(SceneDocument& document,
	const PresentationRuntime& runtime) {
	for (const TransitionPose& pose : runtime.originalPoses) {
		SceneEntity* target = document.FindEntity(pose.entityId);
		if (!target) continue;
		if (pose.propertyMask & 0x1u) target->transform.translate = pose.transform.translate;
		if (pose.propertyMask & 0x2u) target->transform.rotate = pose.transform.rotate;
		if (pose.propertyMask & 0x4u) target->transform.scale = pose.transform.scale;
	}
}

void ScenePrefabAnimationSystem::ResetPresentation(SceneDocument& document, uint64_t entityId) {
	if (!BindPresentationDocument(document)) return;
	const auto found = presentationRuntimes_.find(entityId);
	if (found != presentationRuntimes_.end()) {
		RestorePresentation(document, found->second);
		presentationRuntimes_.erase(found);
	}
	RemovePresentationCompletions(entityId);
}

void ScenePrefabAnimationSystem::UpdatePresentations(SceneDocument& document, float deltaTime) {
	if (!BindPresentationDocument(document)) return;
	const float elapsed = std::isfinite(deltaTime) ? (std::max)(deltaTime, 0.0f) : 0.0f;
	for (auto iterator = presentationRuntimes_.begin(); iterator != presentationRuntimes_.end();) {
		const uint64_t ownerId = iterator->first;
		PresentationRuntime& runtime = iterator->second;
		const SceneEntity* owner = document.FindEntity(ownerId);
		const ScenePrefabAnimationClip* clip = FindPresentationClip(document, ownerId, runtime.clipName);
		bool valid = owner && clip && SceneEntityQuery::IsEntityActiveInHierarchy(document, *owner) &&
			!HasGameplayOwnership(document, runtime.originalPoses);
		// 編集/参照変更で退避していないpropertyへ書き込むことを防ぐ。
		if (valid) {
			size_t targetIndex = 0;
			for (const SceneAnimationTrack& track : clip->tracks) {
				if (track.keyframes.empty()) continue;
				const SceneEntity* target = ResolvePresentationTarget(document, ownerId, track);
				if (targetIndex >= runtime.clipTargets.size() ||
					runtime.clipTargets[targetIndex].entityId != target->id ||
					runtime.clipTargets[targetIndex].propertyMask != TransformPropertyMask(track.property)) {
					valid = false;
					break;
				}
				++targetIndex;
			}
			valid = valid && targetIndex == runtime.clipTargets.size();
		}
		if (!valid) {
			RestorePresentation(document, runtime);
			RemovePresentationCompletions(ownerId);
			iterator = presentationRuntimes_.erase(iterator);
			continue;
		}
		if (runtime.justStarted) {
			runtime.justStarted = false;
		} else if (runtime.playing) {
			runtime.time = (std::min)(runtime.time + elapsed, clip->duration);
			ScenePrefabAnimationEvaluator::ApplyClip(document, ownerId, *clip, runtime.time);
			if (runtime.time >= clip->duration) {
				runtime.playing = false;
				presentationCompletions_.push_back({ ownerId, runtime.clipName, runtime.generation });
			}
		}
		++iterator;
	}
}

std::vector<ScenePresentationCompletion> ScenePrefabAnimationSystem::ConsumePresentationCompletions() {
	std::vector<ScenePresentationCompletion> result;
	result.swap(presentationCompletions_);
	return result;
}

void ScenePrefabAnimationSystem::Clear(SceneDocument* document) {
	if (document && presentationDocumentGeneration_ != 0 &&
		presentationDocumentGeneration_ == document->GetContentGeneration()) {
		for (const auto& entry : presentationRuntimes_) RestorePresentation(*document, entry.second);
		for (const auto& [id, active] : presentationOriginalActive_) {
			if (SceneEntity* owner = document->FindEntity(id)) owner->active = active;
		}
	}
	DiscardPresentations();
	runtimes_.clear();
}
