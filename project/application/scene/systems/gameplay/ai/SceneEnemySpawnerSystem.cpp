// 役割: 敵Prefabの生成/再利用と出現時計を所有し、参加制限を値snapshotで公開する。
#include "SceneEnemySpawnerSystem.h"

#include "../../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../../engine/utility/EditableResourcePath.h"
#include "../../../../../engine/utility/StringUtility.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
	bool WithinBranch(const SceneDocument& document, uint64_t id, uint64_t rootId) {
		for (size_t depth = 0; id && depth <= document.GetEntities().size(); ++depth) {
			if (id == rootId) return true;
			const auto* entity = document.FindEntity(id);
			if (!entity) return false;
			id = entity->parentId;
		}
		return false;
	}
	bool ActiveInHierarchy(const SceneDocument& document, uint64_t id) {
		for (size_t depth = 0; id && depth <= document.GetEntities().size(); ++depth) {
			const auto* entity = document.FindEntity(id);
			if (!entity || !entity->active) return false;
			id = entity->parentId;
		}
		return id == 0;
	}
	bool ValidSettings(const SceneComponent& settings) {
		return (settings.enemySpawnerSpawnAppearance == "Instant" || settings.enemySpawnerSpawnAppearance == "ScaleUp") &&
			(settings.enemySpawnerSpawnCurve == "Linear" || settings.enemySpawnerSpawnCurve == "EaseOutCubic") &&
			std::isfinite(settings.enemySpawnerSpawnStartScale) && settings.enemySpawnerSpawnStartScale >= 0.0f &&
			settings.enemySpawnerSpawnStartScale <= 1.0f && std::isfinite(settings.enemySpawnerSpawnDuration) &&
			(settings.enemySpawnerSpawnAppearance != "ScaleUp" || settings.enemySpawnerSpawnDuration > 0.0f);
	}
	// 外部所有のTransform/戦闘参照を持つPrefabを、半分だけ停止した状態で生成しない。
	bool SupportedBranch(const SceneDocument& document, uint64_t rootId, bool wholeDocument = true) {
		bool hasMesh = false;
		auto internal = [&](uint64_t id, const std::string& name) {
			if (!id && name.empty()) return true;
			const auto* target = id ? document.FindEntity(id) : document.FindEntityByName(name);
			return target && WithinBranch(document, target->id, rootId);
		};
		for (const auto& entity : document.GetEntities()) {
			if (!WithinBranch(document, entity.id, rootId)) {
				if (wholeDocument) return false;
				continue;
			}
			for (const auto& c : entity.components) {
				if (!c.enabled) continue;
				if (c.type == "MeshRenderer" && !c.modelPath.empty()) hasMesh = true;
				if (c.type == "PlayerBehavior" || c.type == "Camera" || c.type == "CameraPath" ||
					c.type == "EnemySpawner" || c.type == "GameFlowDirector" || c.type == "MenuPanel" || c.type == "Projectile") return false;
				if (c.type == "PrefabAnimator") for (const auto& clip : c.prefabAnimationClips) for (const auto& track : clip.tracks)
					if (!internal(track.targetEntityId, track.targetEntityName)) return false;
				if (c.type == "HitBox" && !internal(c.hitBoxOwnerEntityId, c.hitBoxOwnerEntityName)) return false;
				if (c.type == "HurtBox" && !internal(c.hurtBoxStatsEntityId, c.hurtBoxStatsEntityName)) return false;
				if (c.type == "BoneAttachment" && !internal(c.boneAttachmentTargetEntityId, c.boneAttachmentTargetEntityName)) return false;
				if (c.type == "EnemyBehavior" && !internal(c.enemyAttackHitBoxEntityId, c.enemyAttackHitBoxEntityName)) return false;
				if (c.type == "StateMachine") for (const auto& state : c.stateMachineStates) for (const auto& parameter : state.parameters)
					if (parameter.type == "Entity" && !internal(parameter.entityId, parameter.entityName)) return false;
				if (c.type == "AttackSet") for (const auto& attack : c.attackDefinitions) {
					if (!internal(attack.animationTargetEntityId, attack.animationTargetEntityName) ||
						!internal(attack.hitBoxEntityId, attack.hitBoxEntityName)) return false;
					for (const auto& window : attack.hitWindows) if (!internal(window.hitBoxEntityId, window.hitBoxEntityName)) return false;
					for (const auto& effect : attack.effectEvents) if (!internal(effect.spawnEntityId, effect.spawnEntityName)) return false;
				}
			}
		}
		return hasMesh;
	}
}

void SceneEnemySpawnerSystem::Fail(uint64_t spawnerId, Runtime& runtime,
	ScenePreparationDiagnosticCode code, const std::string& detail) {
	if (runtime.stopped) return;
	runtime.stopped = true;
	runtime.cancelPending = true;
	runtime.finiteWave.preparationReady = false;
	if (runtime.finiteWave.state == FiniteWaveState::Running) runtime.finiteWave.state = FiniteWaveState::Failed;
	runtime.finiteWave.diagnostic = detail;
	diagnostic_ = { code, documentGeneration_, spawnerId, ++diagnosticSerial_, detail };
}

void SceneEnemySpawnerSystem::CancelPending(SceneDocument& document, Runtime& runtime) {
	if (!runtime.cancelPending) return;
	for (auto& instance : runtime.instances) {
		if (instance.appearance == AppearanceState::Growing || instance.appearance == AppearanceState::FullSizePending || instance.holdUntilGameplay) {
			if (auto* root = document.FindEntity(instance.rootEntityId)) root->active = false;
			else for (const auto& [id, baseline] : instance.baselineEntities) {
				(void)baseline;
				if (auto* remaining = document.FindEntity(id)) remaining->active = false;
			}
			instance.appearance = AppearanceState::Retired;
			instance.holdUntilGameplay = false;
			instance.scale = 1.0f;
		}
	}
	runtime.cancelPending = false;
}

void SceneEnemySpawnerSystem::AdvanceInstances(SceneDocument& document, uint64_t spawnerId,
	Runtime& runtime, float deltaTime, bool gameplayAllowed) {
	for (auto& instance : runtime.instances) {
		const bool pending = instance.appearance == AppearanceState::Growing ||
			instance.appearance == AppearanceState::FullSizePending || instance.holdUntilGameplay;
		if (!pending) continue;
		bool valid = ActiveInHierarchy(document, instance.rootEntityId);
		size_t branchCount = 0;
		for (const auto& entity : document.GetEntities()) if (WithinBranch(document, entity.id, instance.rootEntityId)) ++branchCount;
		valid = valid && branchCount == instance.baselineEntities.size();
		for (const auto& [id, baseline] : instance.baselineEntities) {
			const auto* entity = document.FindEntity(id);
			valid = valid && entity && entity->parentId == baseline.parentId &&
				(id == instance.rootEntityId || !baseline.active || entity->active);
		}
		if (!valid) {
			Fail(spawnerId, runtime, ScenePreparationDiagnosticCode::InstanceLost, "Spawn branch was removed, disabled or reparented");
			break;
		}
		if (gameplayAllowed) instance.holdUntilGameplay = false;
		if (!(deltaTime > 0.0f)) continue;
		if (instance.appearance == AppearanceState::FullSizePending) {
			instance.appearance = AppearanceState::Ready;
		} else if (instance.appearance == AppearanceState::Growing) {
			instance.elapsed = (std::min)(instance.elapsed + deltaTime, instance.duration);
			const float u = std::clamp(instance.elapsed / instance.duration, 0.0f, 1.0f);
			const float remaining = 1.0f - u;
			const float amount = instance.easeOut ? 1.0f - remaining * remaining * remaining : u;
			instance.scale = instance.startScale + (1.0f - instance.startScale) * amount;
			if (instance.elapsed >= instance.duration) {
				instance.scale = 1.0f;
				instance.appearance = AppearanceState::FullSizePending;
				if (instance.waveGeneration && instance.waveGeneration == runtime.finiteWave.generation) ++runtime.finiteWave.appearanceCompletedCount;
			}
		}
	}
	CancelPending(document, runtime);
}

void SceneEnemySpawnerSystem::RefreshStatus(const SceneDocument& document, Runtime& runtime) {
	auto& status = runtime.finiteWave;
	if (status.state != FiniteWaveState::Running) { status.preparationReady = false; return; }
	status.activeCount = 0;
	status.presentingCount = 0;
	int waveActiveCount = 0;
	for (const auto& instance : runtime.instances) {
		const auto* root = document.FindEntity(instance.rootEntityId);
		if (!root || !root->active) continue;
		++status.activeCount;
		if (instance.waveGeneration == status.generation) ++waveActiveCount;
		if (instance.waveGeneration == status.generation && (instance.appearance == AppearanceState::Growing ||
			instance.appearance == AppearanceState::FullSizePending)) ++status.presentingCount;
	}
	status.preparationReady = status.activatedCount == status.requestedCount &&
		status.appearanceCompletedCount == status.requestedCount && status.presentingCount == 0 &&
		status.activeCount == status.requestedCount && waveActiveCount == status.requestedCount;
	if (status.activatedCount >= status.requestedCount && status.activeCount == 0) {
		status.state = FiniteWaveState::Complete;
		status.preparationReady = false;
	}
}

void SceneEnemySpawnerSystem::Update(SceneDocument& document, float deltaTime) {
	Update(document, SceneSpawnUpdateContext{ deltaTime, deltaTime, true, {} });
}

void SceneEnemySpawnerSystem::Update(SceneDocument& document, const SceneSpawnUpdateContext& context) {
	if (documentGeneration_ != 0 && documentGeneration_ != document.GetContentGeneration()) Clear();
	documentGeneration_ = document.GetContentGeneration();
	if (diagnostic_.serial && diagnostic_.documentGeneration == 0) diagnostic_.documentGeneration = documentGeneration_;
	const float sourceDelta = context.gameplayAllowed ? context.gameplayDeltaTime : context.presentationDeltaTime;
	const float elapsed = std::isfinite(sourceDelta) ? (std::max)(sourceDelta, 0.0f) : 0.0f;
	// 通常列挙から消えたSpawnerも、取消と破損検出だけは取りこぼさない。
	for (auto& [id, runtime] : runtimes_) {
		CancelPending(document, runtime);
		const auto* owner = document.FindEntity(id);
		const auto* component = owner ? SceneEntityQuery::FindEnabledComponent(*owner, "EnemySpawner") : nullptr;
		const bool pending = std::any_of(runtime.instances.begin(), runtime.instances.end(), [](const Instance& i) {
			return i.appearance == AppearanceState::Growing || i.appearance == AppearanceState::FullSizePending || i.holdUntilGameplay;
		});
		if ((pending || runtime.finiteWave.state == FiniteWaveState::Running) && (!component || !ActiveInHierarchy(document, id)))
			Fail(id, runtime, ScenePreparationDiagnosticCode::InvalidReference, "Spawner is no longer active");
		const bool preparing = std::find(context.preparationSpawnerIds.begin(), context.preparationSpawnerIds.end(), id) != context.preparationSpawnerIds.end();
		if (!runtime.stopped) AdvanceInstances(document, id, runtime,
			context.gameplayAllowed || preparing ? elapsed : 0.0f, context.gameplayAllowed);
		CancelPending(document, runtime);
		RefreshStatus(document, runtime);
	}
	std::vector<uint64_t> spawnerIds;
	for (const auto& entity : document.GetEntities()) {
		const auto* spawner = SceneEntityQuery::FindEnabledComponent(entity, "EnemySpawner");
		if (!spawner) continue;
		const auto found = runtimes_.find(entity.id);
		const bool finite = found != runtimes_.end() && found->second.finiteWave.state == FiniteWaveState::Running;
		if (!context.gameplayAllowed && (!finite || std::find(context.preparationSpawnerIds.begin(), context.preparationSpawnerIds.end(), entity.id) == context.preparationSpawnerIds.end())) continue;
		if (!spawner->enemySpawnerAutoStart && !finite) continue;
		if (spawner->enemySpawnerPrefabPath.empty() && spawner->enemySpawnerSpawnAppearance == "Instant" && !finite) continue;
		spawnerIds.push_back(entity.id);
	}
	for (uint64_t spawnerId : spawnerIds) {
		const auto* owner = document.FindEntity(spawnerId);
		const auto* component = owner ? SceneEntityQuery::FindEnabledComponent(*owner, "EnemySpawner") : nullptr;
		if (!component) continue;
		// InstantiatePrefabでDocument配列が再配置される前に、参照していた設定/位置を値で固定する。
		const SceneComponent settings = *component;
		const SceneEntity spawnerEntity = *owner;
		Runtime& runtime = runtimes_[spawnerId];
		if (runtime.stopped) continue;
		if (!ValidSettings(settings)) { Fail(spawnerId, runtime, ScenePreparationDiagnosticCode::InvalidSettings, "Invalid spawn appearance settings"); continue; }
		if (settings.enemySpawnerSpawnAppearance == "ScaleUp") {
			if (!ActiveInHierarchy(document, spawnerId) || settings.enemySpawnerPrefabPath.empty()) {
				Fail(spawnerId, runtime, ScenePreparationDiagnosticCode::InvalidReference, settings.enemySpawnerPrefabPath); continue;
			}
			if (runtime.validatedPrefabPath != settings.enemySpawnerPrefabPath) {
				SceneDocument prefab;
				const auto path = EditableResourcePath::ResolveResource(StringUtility::ToPath(settings.enemySpawnerPrefabPath));
				const bool loaded = prefab.Load(StringUtility::ToUtf8(path));
				uint64_t rootId = 0;
				for (const auto& entity : prefab.GetEntities()) if (entity.parentId == 0) {
					if (rootId) { rootId = 0; break; }
					rootId = entity.id;
				}
				if (!loaded || !rootId) { Fail(spawnerId, runtime, ScenePreparationDiagnosticCode::SpawnFailed, settings.enemySpawnerPrefabPath); continue; }
				if (!SupportedBranch(prefab, rootId)) { Fail(spawnerId, runtime, ScenePreparationDiagnosticCode::UnsupportedPrefab, settings.enemySpawnerPrefabPath); continue; }
				runtime.validatedPrefabPath = settings.enemySpawnerPrefabPath;
			}
			if (context.gameplayAllowed && elapsed <= 0.0f) continue;
		}
		if (!settings.enemySpawnerAutoStart || !context.gameplayAllowed) {
			if (runtime.finiteWave.state == FiniteWaveState::Running) UpdateFiniteWave(document, spawnerEntity, settings, runtime, elapsed);
			continue;
		}
		std::erase_if(runtime.instances, [&](const Instance& instance) { return !document.FindEntity(instance.rootEntityId); });
		int alive = 0;
		Instance* reusable = nullptr;
		for (auto& instance : runtime.instances) {
			const auto* root = document.FindEntity(instance.rootEntityId);
			if (root->active) ++alive; else if (!reusable) reusable = &instance;
		}
		const int initial = (std::max)(settings.enemySpawnerInitialCount, 0);
		const int maxAlive = (std::max)(settings.enemySpawnerMaxAlive, initial);
		const bool needsInitial = runtime.spawnedInitial < initial;
		runtime.timer = (std::max)(runtime.timer - elapsed, 0.0f);
		if (alive >= maxAlive || (!needsInitial && runtime.timer > 0.0f)) continue;
		const float angle = static_cast<float>(runtime.spawnIndex) * 2.39996323f;
		const float radius = (std::max)(settings.enemySpawnerRadius, 0.0f);
		const Vector3& origin = spawnerEntity.transform.translate;
		const Vector3 position = { origin.x + std::cos(angle) * radius, origin.y, origin.z + std::sin(angle) * radius };
		if (!ActivateInstance(document, spawnerId, settings, runtime, reusable, position, false)) continue;
		if (needsInitial) ++runtime.spawnedInitial;
		++runtime.spawnIndex;
		runtime.timer = (std::max)(settings.enemySpawnerInterval, 0.0f);
	}
	for (auto& [id, runtime] : runtimes_) { (void)id; CancelPending(document, runtime); RefreshStatus(document, runtime); }
}

bool SceneEnemySpawnerSystem::ActivateInstance(SceneDocument& document, uint64_t spawnerId,
	const SceneComponent& settings, Runtime& runtime, Instance* reusable, const Vector3& position, bool finite) {
	Instance* activated = reusable;
	if (reusable) {
		if (!RestoreInstance(document, *reusable, position)) activated = nullptr;
	} else {
		const uint64_t id = document.InstantiatePrefab(settings.enemySpawnerPrefabPath, 0, true);
		if (auto* root = document.FindEntity(id)) {
			runtime.instances.push_back(CaptureInstance(document, id));
			activated = &runtime.instances.back();
			root->transform.translate = position;
		}
	}
	if (!activated) {
		if (finite || settings.enemySpawnerSpawnAppearance == "ScaleUp") Fail(spawnerId, runtime, ScenePreparationDiagnosticCode::SpawnFailed, settings.enemySpawnerPrefabPath);
		return false;
	}
	activated->activationSerial = ++activationSerial_;
	activated->waveGeneration = finite ? runtime.finiteWave.generation : 0;
	activated->elapsed = 0.0f;
	activated->startScale = settings.enemySpawnerSpawnStartScale;
	activated->duration = settings.enemySpawnerSpawnDuration;
	activated->easeOut = settings.enemySpawnerSpawnCurve == "EaseOutCubic";
	activated->scaleUp = settings.enemySpawnerSpawnAppearance == "ScaleUp";
	activated->holdUntilGameplay = finite && runtime.holdUntilGameplay;
	activated->appearance = activated->scaleUp ? AppearanceState::Growing : AppearanceState::Ready;
	activated->scale = activated->scaleUp ? activated->startScale : 1.0f;
	if (!reusable && (activated->scaleUp || activated->holdUntilGameplay)) {
		for (const auto& [id, baseline] : activated->baselineEntities) { (void)baseline; resetEntityIds_.push_back(id); }
	}
	// 名前だけの参照は、複数Instanceの展開後に別branchを指す可能性も検証する。
	if (activated->scaleUp && !SupportedBranch(document, activated->rootEntityId, false)) {
		Fail(spawnerId, runtime, ScenePreparationDiagnosticCode::UnsupportedPrefab, settings.enemySpawnerPrefabPath);
		return false;
	}
	if (finite && !activated->scaleUp) ++runtime.finiteWave.appearanceCompletedCount;
	return true;
}

void SceneEnemySpawnerSystem::BeginFiniteWave(uint64_t spawnerEntityId, uint64_t generation, int requestedCount, bool holdUntilGameplay) {
	Runtime& runtime = runtimes_[spawnerEntityId];
	auto& status = runtime.finiteWave;
	if (status.generation == generation && status.state != FiniteWaveState::Idle) return;
	if (status.state == FiniteWaveState::Running) {
		Fail(spawnerEntityId, runtime, ScenePreparationDiagnosticCode::WaveMismatch, "A different finite wave generation is already running"); return;
	}
	if (runtime.stopped) return;
	status = {};
	status.generation = generation;
	status.requestedCount = requestedCount;
	status.state = FiniteWaveState::Running;
	if (generation == 0 || requestedCount <= 0) {
		Fail(spawnerEntityId, runtime, ScenePreparationDiagnosticCode::InvalidSettings, "Finite wave generation and count must be positive"); return;
	}
	runtime.holdUntilGameplay = holdUntilGameplay;
	runtime.timer = 0.0f;
	runtime.spawnIndex = 0;
}

SceneEnemySpawnerSystem::FiniteWaveStatus SceneEnemySpawnerSystem::GetFiniteWaveStatus(uint64_t spawnerEntityId) const {
	const auto found = runtimes_.find(spawnerEntityId);
	return found == runtimes_.end() ? FiniteWaveStatus{} : found->second.finiteWave;
}

void SceneEnemySpawnerSystem::StopFiniteWave(uint64_t spawnerEntityId) {
	const auto found = runtimes_.find(spawnerEntityId);
	if (found == runtimes_.end()) return;
	found->second.finiteWave.state = FiniteWaveState::Idle;
	found->second.finiteWave.activeCount = 0;
	found->second.finiteWave.presentingCount = 0;
	found->second.finiteWave.preparationReady = false;
	found->second.finiteWave.diagnostic.clear();
	found->second.cancelPending = true;
	found->second.timer = 0.0f;
}

void SceneEnemySpawnerSystem::UpdateFiniteWave(SceneDocument& document, const SceneEntity& spawnerEntity,
	const SceneComponent& spawner, Runtime& runtime, float deltaTime) {
	auto& status = runtime.finiteWave;
	if (runtime.holdUntilGameplay && status.requestedCount > spawner.enemySpawnerMaxAlive) {
		Fail(spawnerEntity.id, runtime, ScenePreparationDiagnosticCode::CapacityExceeded, "Initial preparation count exceeds max alive"); return;
	}
	std::erase_if(runtime.instances, [&](const Instance& instance) { return !document.FindEntity(instance.rootEntityId); });
	RefreshStatus(document, runtime);
	if (status.state != FiniteWaveState::Running) return;
	Instance* reusable = nullptr;
	for (auto& instance : runtime.instances) {
		const auto* root = document.FindEntity(instance.rootEntityId);
		if (root && !root->active) { reusable = &instance; break; }
	}
	runtime.timer = (std::max)(runtime.timer - deltaTime, 0.0f);
	if (status.activatedCount >= status.requestedCount || status.activeCount >= (std::max)(spawner.enemySpawnerMaxAlive, 1) || runtime.timer > 0.0f) return;
	const float angle = static_cast<float>(runtime.spawnIndex) * 2.39996323f;
	const float radius = (std::max)(spawner.enemySpawnerRadius, 0.0f);
	const Vector3& origin = spawnerEntity.transform.translate;
	const Vector3 position = { origin.x + std::cos(angle) * radius, origin.y, origin.z + std::sin(angle) * radius };
	if (!ActivateInstance(document, spawnerEntity.id, spawner, runtime, reusable, position, true)) return;
	++status.activatedCount;
	++runtime.spawnIndex;
	runtime.timer = (std::max)(spawner.enemySpawnerInterval, 0.0f);
}

SceneSpawnFrame SceneEnemySpawnerSystem::BuildPresentationFrame(const SceneDocument& document, bool gameplayAllowed) const {
	SceneSpawnFrame frame;
	frame.documentGeneration = document.GetContentGeneration();
	if (frame.documentGeneration != documentGeneration_) return frame;
	for (const auto& [id, runtime] : runtimes_) {
		(void)id;
		for (const auto& instance : runtime.instances) {
			const auto* root = document.FindEntity(instance.rootEntityId);
			const bool growing = instance.appearance == AppearanceState::Growing || instance.appearance == AppearanceState::FullSizePending;
			const bool blocked = growing || instance.appearance == AppearanceState::Retired ||
				(instance.holdUntilGameplay && !gameplayAllowed) || (instance.scaleUp && !ActiveInHierarchy(document, instance.rootEntityId));
			if (root && instance.scaleUp && growing && ActiveInHierarchy(document, root->id)) frame.visuals.push_back({root->id, instance.activationSerial, instance.scale});
			if (!blocked) continue;
			for (const auto& entity : document.GetEntities()) {
				if (instance.baselineEntities.contains(entity.id) || WithinBranch(document, entity.id, instance.rootEntityId)) frame.blockedEntityIds.insert(entity.id);
			}
		}
	}
	return frame;
}

std::vector<uint64_t> SceneEnemySpawnerSystem::ConsumeResetEntityIds() {
	std::vector<uint64_t> result;
	result.swap(resetEntityIds_);
	return result;
}

void SceneEnemySpawnerSystem::Clear() {
	runtimes_.clear();
	resetEntityIds_.clear();
	documentGeneration_ = 0;
	diagnostic_ = {};
	// serialは再開始でも単調にする。残存Objectの再生resetとEditorの通知を取り違えない。
}

SceneEnemySpawnerSystem::Instance SceneEnemySpawnerSystem::CaptureInstance(const SceneDocument& document, uint64_t rootEntityId) const {
	Instance result;
	result.rootEntityId = rootEntityId;
	for (const auto& entity : document.GetEntities()) if (WithinBranch(document, entity.id, rootEntityId)) result.baselineEntities.emplace(entity.id, entity);
	return result;
}

bool SceneEnemySpawnerSystem::RestoreInstance(SceneDocument& document, const Instance& instance, const Vector3& spawnPosition) {
	if (!document.FindEntity(instance.rootEntityId) || instance.baselineEntities.empty()) return false;
	for (const auto& [id, baseline] : instance.baselineEntities) { (void)baseline; if (!document.FindEntity(id)) return false; }
	for (const auto& [id, baseline] : instance.baselineEntities) {
		*document.FindEntity(id) = baseline;
		resetEntityIds_.push_back(id);
	}
	auto* root = document.FindEntity(instance.rootEntityId);
	root->active = true;
	root->transform.translate = spawnPosition;
	return true;
}
