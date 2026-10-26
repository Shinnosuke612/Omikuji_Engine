// 役割: Entity ID、Hierarchy、Entity参照、Scene遷移先の整合性を検証する。
#include "SceneValidator.h"

#include "SceneCatalog.h"
#include "SceneDocument.h"
#include "SceneEntityQuery.h"
#include "SceneUILayout.h"
#include "SceneMenuLayout.h"
#include "SceneInputKey.h"
#include "ScenePrefabAnimationEvaluator.h"
#include "../Audio/Audio.h"
#include "../utility/EditableResourcePath.h"
#include "../utility/StringUtility.h"

#include <algorithm>
#include <exception>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
	// Authoringでは参照の構文/存在だけを確認する。完全なAsset評価はRuntime開始前に行う。
	bool IsTransitionReferenceAvailable(const std::string& value) {
		if (value.size() > 4096 || value.empty() || !value.ends_with(".transition.json") ||
			std::any_of(value.begin(),value.end(),[](unsigned char c) { return c < 32 || c == 127 || c == ':'; })) return false;
		try {
			const auto path = StringUtility::ToPath(value);
			if (path.has_root_path() || path.begin()==path.end() || *path.begin()!=L"resources") return false;
			for (const auto& part : path) if (part==L"..") return false;
			std::error_code error;
			return std::filesystem::is_regular_file(EditableResourcePath::ResolveResource(path),error) && !error;
		} catch (const std::exception&) { return false; }
	}
	bool HasErrors(const std::vector<SceneValidationIssue>& issues) {
		for (const SceneValidationIssue& issue : issues) {
			if (issue.severity == SceneValidationSeverity::Error) {
				return true;
			}
		}
		return false;
	}

	bool IsValidAudioSpatialMode(const std::string& mode) {
		return mode == "TwoD" || mode == "ThreeD" ||
			mode == "ThreeDPointDownmix" || mode == "ThreeDStereoArea";
	}

	std::string DescribeAudioChannelMismatch(
		const std::string& spatialMode,
		uint32_t channelCount
	) {
		if (spatialMode == "ThreeD") {
			if (channelCount == 2) {
				return "AudioSource clip has 2 channels, but ThreeD Point requires mono. "
					"Use ThreeD Point Downmix or ThreeD Stereo Area.";
			}
			return "AudioSource clip has " + std::to_string(channelCount) +
				" channels, but ThreeD Point requires mono.";
		}
		if (channelCount == 1) {
			return "AudioSource clip has mono, but " + spatialMode +
				" requires stereo. Use ThreeD Point.";
		}
		return "AudioSource clip has " + std::to_string(channelCount) +
			" channels, but " + spatialMode + " requires stereo.";
	}
}

bool SceneValidator::ValidateDocument(
	const SceneDocument& document,
	const SceneCatalog* catalog,
	const std::string& sceneId,
	const std::string& filePath,
	std::vector<SceneValidationIssue>& issues
) {
	const size_t firstIssueIndex = issues.size();
	auto addIssue = [
		&issues,
		&sceneId,
		&filePath
	](SceneValidationSeverity severity, uint64_t entityId, std::string message) {
		issues.push_back({ severity, sceneId, filePath, entityId, std::move(message) });
	};

	std::unordered_map<uint64_t, const SceneEntity*> entitiesById;
	// Menu参照は同一DocumentのIDだけで検証し、名前fallbackで接続先を補わない。
	auto isWithinMenu = [&document](uint64_t childId, uint64_t rootId) {
		const SceneEntity* current = document.FindEntity(childId);
		for (size_t depth = 0; current && depth <= document.GetEntities().size(); ++depth) {
			if (current->id == rootId) return true;
			current = document.FindEntity(current->parentId);
		}
		return false;
	};
	std::unordered_set<uint64_t> assignedMenuItems;
	// 新UIの可視性はCanvas/配置の無効化も含む。循環するUI祖先を旧active走査へ渡さない。
	auto isMenuEntityActive = [&document](const SceneEntity& entity) {
		if (SceneUILayout::IsUIEntity(document, entity)) {
			SceneUILayout::Layout layout;
			return SceneUILayout::Resolve(document, entity.id, layout) && layout.visible;
		}
		return SceneEntityQuery::IsEntityActiveInHierarchy(document, entity);
	};
	std::unordered_set<uint64_t> assignedMenuMarkers;
	std::unordered_set<std::string> uiKeyBindings;
	size_t initiallyOpenMenus = 0;
	size_t gamePauseCount = 0;
	for (const SceneEntity& owner : document.GetEntities()) {
		for (const SceneComponent& setting : owner.components) {
			if (setting.type == "EventTrigger") {
				for (const auto& binding : setting.eventBindings) for (const auto& action : binding.actions) {
					if (action.type == "RestartScene" && binding.executionDomain != "Gameplay")
						addIssue(SceneValidationSeverity::Error, owner.id, "SceneRestart:InvalidDomain");
				}
			}
			if (setting.type == "GameFlowDirector") {
				auto flowError = [&](const char* code) {
					addIssue(SceneValidationSeverity::Error, owner.id, std::string("FlowPreparation:") + code);
				};
				auto componentAt = [&](uint64_t id, const char* type) -> const SceneComponent* {
					const auto* entity = document.FindEntity(id);
					return entity ? SceneEntityQuery::FindEnabledComponent(*entity, type) : nullptr;
				};
				auto active = [&](uint64_t id) {
					for (size_t depth = 0; id && depth <= document.GetEntities().size(); ++depth) {
						const auto* entity = document.FindEntity(id);
						if (!entity || !entity->active) return false;
						id = entity->parentId;
					}
					return id == 0;
				};
				auto below = [&](uint64_t id, uint64_t rootId) {
					for (size_t depth = 0; id && depth <= document.GetEntities().size(); ++depth) {
						if (id == rootId) return true;
						const auto* entity = document.FindEntity(id);
						id = entity ? entity->parentId : 0;
					}
					return false;
				};
				auto motionAt = [&](uint64_t id, const std::string& clipId) {
					const auto* motion = componentAt(id, "TextMotion");
					return motion && !clipId.empty() && std::any_of(motion->textMotionClips.begin(), motion->textMotionClips.end(),
						[&](const auto& clip) { return clip.id == clipId; });
				};
				const bool briefing = setting.gameFlowStartSequence == "BriefingSpawnCountdown";
				if ((setting.gameFlowStartSequence != "CountdownFirst" && !briefing) ||
					setting.gameFlowCountdownStart < 1 || setting.gameFlowCountdownStart > 9 ||
					!std::isfinite(setting.gameFlowIntroSeconds) || (briefing && setting.gameFlowIntroSeconds <= 0.0f) ||
					!std::isfinite(setting.gameFlowCountdownStepSeconds) || setting.gameFlowCountdownStepSeconds <= 0.0f ||
					!std::isfinite(setting.gameFlowStartCueSeconds) || setting.gameFlowStartCueSeconds < 0.0f ||
					!std::isfinite(setting.gameFlowInterPhaseDelaySeconds) || setting.gameFlowInterPhaseDelaySeconds < 0.0f ||
					!std::isfinite(setting.gameFlowResultRevealDelaySeconds) || setting.gameFlowResultRevealDelaySeconds < 0.0f ||
					!std::isfinite(setting.gameFlowTimerDisplayStepSeconds) || setting.gameFlowTimerDisplayStepSeconds <= 0.0f)
					flowError("InvalidSettings");
				if (setting.enabled) {
					size_t directors = 0;
					for (const auto& entity : document.GetEntities()) if (componentAt(entity.id, "GameFlowDirector")) ++directors;
					if (directors != 1) flowError("MultipleDirectors");
				}
				const uint64_t textIds[] = {setting.gameFlowCountdownTextEntityId, setting.gameFlowPhaseTextEntityId,
					setting.gameFlowTimerTextEntityId, setting.gameFlowResultTimeTextEntityId};
				std::unordered_set<uint64_t> textTargets;
				for (uint64_t id : textIds) {
					if (!componentAt(id, "TextRenderer") || !textTargets.insert(id).second) flowError("InvalidReference");
				}
				if (setting.gameFlowRemainingTextEntityId && (!componentAt(setting.gameFlowRemainingTextEntityId, "TextRenderer") ||
					!textTargets.insert(setting.gameFlowRemainingTextEntityId).second)) flowError("InvalidReference");
				if (!motionAt(setting.gameFlowCountdownTextEntityId, setting.gameFlowCountdownMotionClipId) ||
					!motionAt(setting.gameFlowPhaseTextEntityId, setting.gameFlowPhaseMotionClipId) ||
					!motionAt(setting.gameFlowResultTimeTextEntityId, setting.gameFlowResultMotionClipId) ||
					!document.FindEntity(setting.gameFlowResultRootEntityId) || below(owner.id, setting.gameFlowResultRootEntityId) ||
					(setting.gameFlowRemainingTextEntityId && !active(setting.gameFlowRemainingTextEntityId))) flowError("InvalidReference");
				if (briefing) {
					const uint64_t introId = setting.gameFlowIntroTextEntityId ? setting.gameFlowIntroTextEntityId : setting.gameFlowCountdownTextEntityId;
					const auto* introEntity = document.FindEntity(introId);
					const auto* text = componentAt(introId, "TextRenderer");
					bool visible = introEntity && text && active(introId);
					if (visible && SceneUILayout::IsUIEntity(document, *introEntity)) {
						SceneUILayout::Layout layout;
						visible = SceneUILayout::Resolve(document, introId, layout) && layout.visible && layout.renderSpace == "ScreenOverlay";
					} else visible = visible && text->textRenderSpace == "ScreenOverlay";
					if (!visible || (!setting.gameFlowIntroMotionClipId.empty() && !motionAt(introId, setting.gameFlowIntroMotionClipId))) flowError("IntroReference");
					for (uint64_t id : {setting.gameFlowPhaseTextEntityId, setting.gameFlowTimerTextEntityId,
						setting.gameFlowRemainingTextEntityId, setting.gameFlowResultTimeTextEntityId})
						if (introId && introId == id) flowError("IntroConflict");
					for (const auto& entity : document.GetEntities()) {
						const auto* events = componentAt(entity.id, "EventTrigger");
						if (!events || !active(entity.id)) continue;
						for (const auto& binding : events->eventBindings) for (const auto& action : binding.actions) {
							const auto* target = action.targetEntityId ? document.FindEntity(action.targetEntityId) :
								action.targetEntityName.empty() ? &entity : document.FindEntityByName(action.targetEntityName);
							if (target && target->id == introId && (action.type == "PlayTextMotion" || action.type == "StopTextMotion" ||
								action.type == "ResetTextMotion" || action.type == "SetEntityActive")) flowError("EventConflict");
						}
					}
				}
				std::unordered_set<std::string> phaseIds;
				if (setting.gameFlowPhases.empty()) flowError("InvalidPhase");
				for (size_t phaseIndex = 0; phaseIndex < setting.gameFlowPhases.size(); ++phaseIndex) {
					const auto& phase = setting.gameFlowPhases[phaseIndex];
					if (phase.id.empty() || !phaseIds.insert(phase.id).second || phase.waves.empty()) flowError("InvalidPhase");
					std::unordered_set<uint64_t> spawnerIds;
					for (const auto& wave : phase.waves) {
						const auto* spawner = componentAt(wave.spawnerEntityId, "EnemySpawner");
						if (!spawner || spawner->enemySpawnerAutoStart || spawner->enemySpawnerPrefabPath.empty() ||
							wave.count <= 0 || !spawnerIds.insert(wave.spawnerEntityId).second || (briefing && !active(wave.spawnerEntityId))) flowError("WaveReference");
						if (briefing && phaseIndex == 0 && spawner && wave.count > spawner->enemySpawnerMaxAlive) flowError("CapacityExceeded");
					}
				}
			}
			if (setting.type == "EnemySpawner") {
				auto spawnError = [&](const char* code) {
					addIssue(SceneValidationSeverity::Error, owner.id, std::string("SpawnPreparation:") + code);
				};
				const bool validSettings = (setting.enemySpawnerSpawnAppearance == "Instant" || setting.enemySpawnerSpawnAppearance == "ScaleUp") &&
					(setting.enemySpawnerSpawnCurve == "Linear" || setting.enemySpawnerSpawnCurve == "EaseOutCubic") &&
					std::isfinite(setting.enemySpawnerSpawnStartScale) && setting.enemySpawnerSpawnStartScale >= 0.0f && setting.enemySpawnerSpawnStartScale <= 1.0f &&
					std::isfinite(setting.enemySpawnerSpawnDuration) && (setting.enemySpawnerSpawnAppearance != "ScaleUp" || setting.enemySpawnerSpawnDuration > 0.0f);
				if (!validSettings) spawnError("InvalidSettings");
				if (validSettings && setting.enabled && setting.enemySpawnerSpawnAppearance == "ScaleUp") {
					// ID再割当て前に参照を確認する。Assetを保存/移行しない。
					SceneDocument prefab;
					const auto path = EditableResourcePath::ResolveResource(StringUtility::ToPath(setting.enemySpawnerPrefabPath));
					const bool loaded = !setting.enemySpawnerPrefabPath.empty() && prefab.Load(StringUtility::ToUtf8(path));
					uint64_t root = 0;
					for (const auto& entity : prefab.GetEntities()) if (entity.parentId == 0) {
						if (root) { root = 0; break; }
						root = entity.id;
					}
					if (!loaded || !root) spawnError("InvalidReference");
					else {
						auto internal = [&](uint64_t id, const std::string& name) {
							if (!id && name.empty()) return true;
							const auto* target = id ? prefab.FindEntity(id) : prefab.FindEntityByName(name);
							if (!target) return false;
							uint64_t current = target->id;
							for (size_t depth = 0; current && depth <= prefab.GetEntities().size(); ++depth) {
								if (current == root) return true;
								const auto* parent = prefab.FindEntity(current);
								current = parent ? parent->parentId : 0;
							}
							return false;
						};
						bool supported = true, hasMesh = false;
						for (const auto& entity : prefab.GetEntities()) {
							if (!internal(entity.id, {})) { supported = false; continue; }
							for (const auto& c : entity.components) {
								if (!c.enabled) continue;
								if (c.type == "MeshRenderer" && !c.modelPath.empty()) hasMesh = true;
								if (c.type == "PlayerBehavior" || c.type == "Camera" || c.type == "CameraPath" || c.type == "EnemySpawner" ||
									c.type == "GameFlowDirector" || c.type == "MenuPanel" || c.type == "Projectile") supported = false;
								if (c.type == "PrefabAnimator") for (const auto& clip : c.prefabAnimationClips) for (const auto& track : clip.tracks)
									supported &= internal(track.targetEntityId, track.targetEntityName);
								if (c.type == "HitBox") supported &= internal(c.hitBoxOwnerEntityId, c.hitBoxOwnerEntityName);
								if (c.type == "HurtBox") supported &= internal(c.hurtBoxStatsEntityId, c.hurtBoxStatsEntityName);
								if (c.type == "BoneAttachment") supported &= internal(c.boneAttachmentTargetEntityId, c.boneAttachmentTargetEntityName);
								if (c.type == "EnemyBehavior") supported &= internal(c.enemyAttackHitBoxEntityId, c.enemyAttackHitBoxEntityName);
								if (c.type == "StateMachine") for (const auto& state : c.stateMachineStates) for (const auto& parameter : state.parameters)
									if (parameter.type == "Entity") supported &= internal(parameter.entityId, parameter.entityName);
								if (c.type == "AttackSet") for (const auto& attack : c.attackDefinitions) {
									supported &= internal(attack.animationTargetEntityId, attack.animationTargetEntityName) && internal(attack.hitBoxEntityId, attack.hitBoxEntityName);
									for (const auto& window : attack.hitWindows) supported &= internal(window.hitBoxEntityId, window.hitBoxEntityName);
									for (const auto& effect : attack.effectEvents) supported &= internal(effect.spawnEntityId, effect.spawnEntityName);
								}
							}
						}
						if (!supported || !hasMesh) spawnError("UnsupportedPrefab");
					}
				}
			}
			auto menuError = [&](const std::string& message) {
				addIssue(SceneValidationSeverity::Error, owner.id, message);
			};
			auto requireMenuTarget = [&](uint64_t id, const char* type, const std::string& label) {
				const SceneEntity* target = document.FindEntity(id);
				const SceneComponent* targetComponent = target
					? SceneEntityQuery::FindEnabledComponent(*target, type) : nullptr;
				if (!id || !targetComponent) menuError(label + " requires an explicit enabled " + type + " Entity ID");
				return targetComponent;
			};
			if (setting.type == "MenuPanel") {
				// 非表示で保管するメニューも構造は検証するが、子の表示状態は要求しない。
				const bool panelVisible = setting.enabled && isMenuEntityActive(owner);
				if ((setting.menuLayoutMode != "Hierarchical" && setting.menuLayoutMode != "CanvasItems") ||
					(setting.menuInputMode != "Events" && setting.menuInputMode != "Standard")) menuError("MenuRuntime:StandardMode");
				std::vector<SceneMenuLayout::ItemPosition> positions;
				std::string layoutError;
				if (!SceneMenuLayout::EvaluatePositions(document, owner, setting, positions, layoutError)) {
					menuError(layoutError);
				} else {
					for (size_t i = 1; i < positions.size(); ++i) {
						if (SceneMenuLayout::Coordinate(positions[i - 1], setting) > SceneMenuLayout::Coordinate(positions[i], setting)) {
							menuError("Menu selection order is out of date. Save the document in the Editor.");
							break;
						}
					}
				}
				if (panelVisible && setting.menuInitiallyOpen) {
					if (++initiallyOpenMenus > 1) menuError("Only one active MenuPanel may be initially open");
				}
				if (setting.menuNavigationAxis != "Vertical" && setting.menuNavigationAxis != "Horizontal") menuError("MenuPanel navigationAxis must be Vertical or Horizontal");
				if (setting.menuCancelMode != "Close" && setting.menuCancelMode != "InvokeItem" && setting.menuCancelMode != "None") menuError("MenuPanel has an invalid cancelMode");
				std::unordered_set<uint64_t> localItems;
				bool hasSelectableItem = false;
				for (uint64_t id : setting.menuItemEntityIds) {
					const SceneComponent* item = requireMenuTarget(id, "MenuItem", "MenuPanel item");
					if (!localItems.insert(id).second) menuError("MenuPanel contains a duplicate item Entity ID");
					if (!assignedMenuItems.insert(id).second) menuError("MenuItem is listed more than once or shared between panels");
					if (id == owner.id || !isWithinMenu(id, owner.id)) menuError("MenuPanel items must be descendants of their panel");
					const SceneEntity* itemEntity = document.FindEntity(id);
					hasSelectableItem |= item && item->menuInteractable && itemEntity && isMenuEntityActive(*itemEntity);
				}
				auto validateChoice = [&](uint64_t id, const char* label) {
					const SceneComponent* item = requireMenuTarget(id, "MenuItem", label);
					const SceneEntity* itemEntity = document.FindEntity(id);
					if (!localItems.contains(id) || !item || !item->menuInteractable || !itemEntity ||
						(panelVisible && !isMenuEntityActive(*itemEntity))) {
						menuError(std::string(label) + " must be a selectable item in this panel");
					}
				};
				if (setting.menuInitialItemEntityId) validateChoice(setting.menuInitialItemEntityId, "MenuPanel initial selection");
				if (setting.menuCancelMode == "InvokeItem") validateChoice(setting.menuCancelItemEntityId, "MenuPanel cancel item");
				if (!hasSelectableItem) addIssue(SceneValidationSeverity::Warning, owner.id, "Menu runtime: no selectable item");
				if (panelVisible && !hasSelectableItem && setting.menuCancelMode != "Close") menuError("MenuPanel has no selectable item or usable cancel operation");
				const SceneEntity* parent = document.FindEntity(owner.parentId);
				for (size_t depth = 0; parent && depth <= document.GetEntities().size(); ++depth) {
					if (SceneEntityQuery::HasComponent(*parent, "MenuPanel")) { menuError("MenuPanel roots must not be nested in the hierarchy"); break; }
					parent = document.FindEntity(parent->parentId);
				}
			} else if (setting.type == "MenuItem") {
				if (!std::isfinite(setting.menuTextPadding.x) || !std::isfinite(setting.menuTextPadding.y) ||
					!std::isfinite(setting.menuTextOffset.x) || !std::isfinite(setting.menuTextOffset.y) ||
					setting.menuTextPadding.x < 0 || setting.menuTextPadding.y < 0) menuError("MenuRuntime:TextPlacement");
				const uint64_t labelId = setting.menuLabelEntityId ? setting.menuLabelEntityId : owner.id;
				const SceneEntity* label = document.FindEntity(labelId);
				const SceneComponent* text = label ? SceneEntityQuery::FindEnabledComponent(*label, "TextRenderer") : nullptr;
				if (!isWithinMenu(labelId, owner.id)) menuError("MenuItem label must be the item itself or a descendant");
				if (text && label) {
					if (SceneUILayout::IsUIEntity(document, *label)) {
						SceneUILayout::Layout layout;
						if (!SceneUILayout::Resolve(document, label->id, layout) || layout.renderSpace != "ScreenOverlay") {
							menuError("MenuItem label requires a valid ScreenOverlay UICanvas");
						}
					} else if (text->textRenderSpace != "ScreenOverlay") {
						menuError("MenuItem label TextRenderer must use ScreenOverlay");
					}
				}
				if (!text) addIssue(SceneValidationSeverity::Warning, owner.id, "MenuItem label has no enabled TextRenderer; configure a visual label or marker");
				if (setting.menuSelectionMarkerEntityId) {
					const SceneEntity* marker = document.FindEntity(setting.menuSelectionMarkerEntityId);
					if (!marker || marker->id == owner.id || !isWithinMenu(marker->id, owner.id) ||
						(!SceneEntityQuery::FindEnabledComponent(*marker, "SpriteRenderer") && !SceneEntityQuery::FindEnabledComponent(*marker, "TextRenderer"))) addIssue(SceneValidationSeverity::Warning, owner.id, "MenuItem marker must be a descendant SpriteRenderer or TextRenderer");
					bool invalidBranch = isWithinMenu(labelId, setting.menuSelectionMarkerEntityId);
					for (const auto& descendant : document.GetEntities()) {
						if (!isWithinMenu(descendant.id, setting.menuSelectionMarkerEntityId)) continue;
						invalidBranch |= SceneEntityQuery::FindComponent(descendant, "MenuItem") || SceneEntityQuery::FindComponent(descendant, "MenuPanel");
					}
					for (const auto& other : document.GetEntities()) {
						const auto* otherItem = SceneEntityQuery::FindComponent(other, "MenuItem");
						if (otherItem && (isWithinMenu(otherItem->menuLabelEntityId ? otherItem->menuLabelEntityId : other.id, setting.menuSelectionMarkerEntityId) ||
							(other.id != owner.id && otherItem->menuSelectionMarkerEntityId == setting.menuSelectionMarkerEntityId))) invalidBranch = true;
					}
					if (invalidBranch) addIssue(SceneValidationSeverity::Warning, owner.id, "Menu runtime: marker branch");
					// 不正な装飾だけを無視するため、Marker診断でScene全体の読込を拒否しない。
					if (!assignedMenuMarkers.insert(setting.menuSelectionMarkerEntityId).second) addIssue(SceneValidationSeverity::Warning, owner.id, "MenuItem selection marker must not be shared");
				}
				const Vector4& color = setting.menuSelectedTextColor;
				if (!std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z) || !std::isfinite(color.w) ||
					color.x < 0 || color.x > 1 || color.y < 0 || color.y > 1 || color.z < 0 || color.z > 1 || color.w < 0 || color.w > 1) menuError("MenuItem selectedTextColor must contain finite values in [0, 1]");
			} else if (setting.type == "GamePause") {
				if (setting.enabled && isMenuEntityActive(owner) && ++gamePauseCount > 1) menuError("Only one active GamePause is allowed per scene");
				const SceneComponent* panel = requireMenuTarget(setting.gamePausePanelEntityId, "MenuPanel", "GamePause panel");
				if (panel && panel->menuInitiallyOpen) menuError("GamePause panel must start closed");
			} else if (setting.type == "EventTrigger") {
				for (const SceneEventBinding& binding : setting.eventBindings) {
					if (binding.executionDomain != "Gameplay" && binding.executionDomain != "UI") menuError("Event executionDomain must be Gameplay or UI");
					const bool ui = binding.executionDomain == "UI";
					if (ui) {
						const bool keyBinding = binding.triggerType == "OnKeyPressed";
						if (keyBinding) for (const auto& other : document.GetEntities()) {
							const auto* events = SceneEntityQuery::FindEnabledComponent(other, "EventTrigger");
							if (!events) continue;
							for (const auto& gameplay : events->eventBindings) if (gameplay.executionDomain == "Gameplay" && gameplay.triggerType == "OnKeyPressed" &&
								ResolveSceneInputKey(gameplay.triggerKey) == ResolveSceneInputKey(binding.triggerKey))
								addIssue(SceneValidationSeverity::Warning, owner.id, "Menu runtime: key conflict");
						}
						if (!std::isfinite(binding.cooldown) || binding.cooldown < 0) menuError("UI event cooldown must be finite and non-negative");
						if (binding.triggerType != "OnKeyPressed" && binding.triggerType != "OnMenuItemConfirmed" && binding.triggerType != "OnMenuCancelled") menuError("Unsupported UI event condition: " + binding.triggerType);
						if (binding.uiInputScope != "WhenNoMenu" && binding.uiInputScope != "FocusedMenu") menuError("Invalid UI input scope");
						if (binding.triggerType == "OnMenuItemConfirmed") requireMenuTarget(binding.targetEntityId, "MenuItem", "UI confirmation");
						if (binding.triggerType == "OnMenuCancelled" || (binding.triggerType == "OnKeyPressed" && binding.uiInputScope == "FocusedMenu")) requireMenuTarget(binding.targetEntityId, "MenuPanel", "UI event target");
						if (binding.triggerType == "OnKeyPressed" && setting.enabled && SceneEntityQuery::IsEntityActiveInHierarchy(document, owner)) {
							const auto key = ResolveSceneInputKey(binding.triggerKey);
							const auto* target = document.FindEntity(binding.targetEntityId);
							const auto* panel = target ? SceneEntityQuery::FindEnabledComponent(*target, "MenuPanel") : nullptr;
							if (binding.uiInputScope == "FocusedMenu" && panel && panel->menuInputMode == "Standard") {
								const bool horizontal = panel->menuNavigationAxis == "Horizontal";
								if (key && (key == ResolveSceneInputKey("ENTER") || key == ResolveSceneInputKey("ESCAPE") ||
									key == ResolveSceneInputKey(horizontal ? "LEFT" : "UP") || key == ResolveSceneInputKey(horizontal ? "RIGHT" : "DOWN")))
									addIssue(SceneValidationSeverity::Warning, owner.id, "MenuRuntime:ReservedKey");
							}
							const std::string identity = binding.uiInputScope + ":" + std::to_string(binding.uiInputScope == "FocusedMenu" ? binding.targetEntityId : 0) + ":" + std::to_string(key);
							if (key && !uiKeyBindings.insert(identity).second) addIssue(SceneValidationSeverity::Warning, owner.id, "Duplicate UI key binding in the same input scope");
						}
					} else if (binding.triggerType == "OnMenuItemConfirmed" || binding.triggerType == "OnMenuCancelled") menuError("Menu notifications require the UI execution domain");
					for (const SceneEventAction& action : binding.actions) {
						if (action.type == "PauseGame" || action.type == "ResumeGame" || action.type == "RequestApplicationExit")
							addIssue(SceneValidationSeverity::Warning, owner.id, "MenuRuntime:UnsupportedAction " + action.type);
						const bool panelAction = action.type == "OpenMenu" || action.type == "CloseMenu" || action.type == "SelectPreviousMenuItem" || action.type == "SelectNextMenuItem" || action.type == "ConfirmMenuItem" || action.type == "CancelMenu";
						const bool pauseAction = action.type == "PauseGame" || action.type == "ResumeGame";
						const bool newAction = panelAction || pauseAction || action.type == "RequestApplicationExit";
						if (panelAction) requireMenuTarget(action.targetEntityId, "MenuPanel", action.type);
						if (pauseAction) requireMenuTarget(action.targetEntityId, "GamePause", action.type);
						if (!ui && newAction && action.type != "OpenMenu" && action.type != "PauseGame") menuError("This menu action requires the UI domain: " + action.type);
						if (ui) {
							const bool audioAction = action.type == "PlayAudio" || action.type == "StopAudio" || action.type == "PauseAudio" || action.type == "ResumeAudio";
							const bool textAction = action.type == "PlayTextMotion" || action.type == "StopTextMotion" || action.type == "ResetTextMotion";
							if (!newAction && !audioAction && !textAction && action.type != "SceneTransition") menuError("Unsupported UI action: " + action.type);
							if (audioAction) requireMenuTarget(action.targetEntityId, "AudioSource", action.type);
							if (textAction) requireMenuTarget(action.targetEntityId, "TextMotion", action.type);
							if (binding.triggerType == "OnMenuCancelled" && (action.type == "CloseMenu" || action.type == "ResumeGame" || action.type == "SceneTransition" || action.type == "RequestApplicationExit")) {
								const SceneEntity* panelEntity = document.FindEntity(binding.targetEntityId);
								const SceneComponent* panel = panelEntity ? SceneEntityQuery::FindEnabledComponent(*panelEntity, "MenuPanel") : nullptr;
								if (panel && panel->menuCancelMode != "None") addIssue(SceneValidationSeverity::Warning, owner.id, "OnMenuCancelled duplicates the panel's configured cancel behavior");
							}
						}
					}
				}
			}
		}
	}
	// UI構造はdisabledでも検証する。描画時と同じResolverで旧方式への漏れも防ぐ。
	for (const SceneEntity& entity : document.GetEntities()) {
		for (const char* type : { "UICanvas", "UITransform" }) {
			const size_t count = std::count_if(entity.components.begin(), entity.components.end(),
				[type](const SceneComponent& component) { return component.type == type; });
			if (count > 1) addIssue(SceneValidationSeverity::Error, entity.id, std::string(type) + " must be unique on an Entity");
		}
		for (const SceneComponent& component : entity.components) {
			std::string reason;
			if ((component.type == "UICanvas" && !SceneUILayout::ValidateCanvas(component, reason)) ||
				(component.type == "UITransform" && !SceneUILayout::ValidateTransform(component, reason))) {
				addIssue(SceneValidationSeverity::Error, entity.id, reason);
			}
		}
		if (SceneUILayout::IsUIEntity(document, entity) &&
			(SceneEntityQuery::FindComponent(entity, "UICanvas") || SceneEntityQuery::FindComponent(entity, "UITransform") ||
				SceneEntityQuery::FindComponent(entity, "TextRenderer") || SceneEntityQuery::FindComponent(entity, "SpriteRenderer"))) {
			SceneUILayout::Layout layout;
			std::string reason;
			if (!SceneUILayout::Resolve(document, entity.id, layout, &reason)) {
				const bool detachedPrefab = filePath.ends_with(".prefab.json") &&
					reason == "UITransform requires a UICanvas ancestor";
				addIssue(detachedPrefab ? SceneValidationSeverity::Warning : SceneValidationSeverity::Error, entity.id, reason);
			}
			if (reason.empty() && layout.renderSpace != "ScreenOverlay" &&
				(SceneEntityQuery::FindComponent(entity, "MenuPanel") || SceneEntityQuery::FindComponent(entity, "MenuItem"))) {
				addIssue(SceneValidationSeverity::Error, entity.id, "Menu UI requires ScreenOverlay UICanvas");
			}
			if (SceneEntityQuery::FindComponent(entity, "UICanvas") &&
				(SceneEntityQuery::FindComponent(entity, "SpriteRenderer") || SceneEntityQuery::FindComponent(entity, "TextRenderer"))) {
				addIssue(SceneValidationSeverity::Error, entity.id, "Canvas visuals require a child Entity with UITransform");
			}
			if (const auto* placement = SceneEntityQuery::FindComponent(entity, "UITransform");
				placement && SceneEntityQuery::FindComponent(entity, "TextRenderer") &&
				(placement->uiSize.x > 4096.0f || placement->uiSize.y > 4096.0f)) {
				addIssue(SceneValidationSeverity::Error, entity.id, "UI text layout exceeds the existing rasterizer limit of 4096");
			}
		}
	}
	for (const SceneEntity& entity : document.GetEntities()) {
		if (entity.id == 0) {
			addIssue(
				SceneValidationSeverity::Error,
				0,
				"Entity ID must be greater than zero"
			);
			continue;
		}
		if (!entitiesById.emplace(entity.id, &entity).second) {
			addIssue(
				SceneValidationSeverity::Error,
				entity.id,
				"Duplicate Entity ID"
			);
		}
	}

	std::unordered_set<uint64_t> reportedCycles;
	uint32_t directionalLightCount = 0;
	uint32_t pointLightCount = 0;
	uint32_t spotLightCount = 0;
	uint32_t spotShadowCount = 0;
	uint32_t cameraSwitcherCount = 0;
	uint32_t activeAudioListenerCount = 0;
	uint64_t firstAudioListenerEntityId = 0;
	uint32_t persistentBgmPlayOnStartCount = 0;
	uint64_t firstPersistentBgmEntityId = 0;
	std::unordered_map<uint64_t, std::unordered_set<uint64_t>> prefabLocalIds;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!entity.teamName.empty() && !document.FindTeam(entity.teamName)) {
			addIssue(
				SceneValidationSeverity::Error,
				entity.id,
				"Entity references an unknown Team: " + entity.teamName
			);
		}
		if (entity.parentId == entity.id && entity.id != 0) {
			addIssue(
				SceneValidationSeverity::Error,
				entity.id,
				"Entity cannot be its own parent"
			);
		} else if (entity.parentId != 0 && !entitiesById.contains(entity.parentId)) {
			addIssue(
				SceneValidationSeverity::Error,
				entity.id,
				"Parent Entity does not exist: " + std::to_string(entity.parentId)
			);
		}
		std::unordered_set<uint64_t> componentLocalIds;
		for (const SceneComponent& component : entity.components) {
			if (component.localId == 0) {
				addIssue(
					SceneValidationSeverity::Error,
					entity.id,
					"Component local ID must be greater than zero: " +
						component.type
				);
			} else if (!componentLocalIds.insert(component.localId).second) {
				addIssue(
					SceneValidationSeverity::Error,
					entity.id,
					"Entity contains a duplicate Component local ID: " +
						std::to_string(component.localId)
				);
			}
		}
		const bool hasPrefabMetadata =
			!entity.prefabLinks.empty() ||
			!entity.prefabAssetId.empty() ||
			!entity.prefabSourcePath.empty() ||
			entity.prefabInstanceRootId != 0 ||
			entity.prefabLocalId != 0;
		if (hasPrefabMetadata) {
			if (
				(
					entity.prefabAssetId.empty() &&
					entity.prefabSourcePath.empty()
				) ||
				entity.prefabInstanceRootId == 0 ||
				entity.prefabLocalId == 0
			) {
				addIssue(
					SceneValidationSeverity::Error,
					entity.id,
					"Prefab instance metadata is incomplete"
				);
			} else {
				const auto root = entitiesById.find(entity.prefabInstanceRootId);
				if (root == entitiesById.end()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Prefab instance root does not exist: " +
						std::to_string(entity.prefabInstanceRootId)
					);
				} else {
					const bool assetLinkMatches = !entity.prefabAssetId.empty()
						? root->second->prefabAssetId == entity.prefabAssetId
						: root->second->prefabAssetId.empty() &&
							root->second->prefabSourcePath ==
								entity.prefabSourcePath;
					if (
						root->second->prefabInstanceRootId !=
							entity.prefabInstanceRootId ||
						!assetLinkMatches
					) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Prefab instance root metadata does not match"
					);
					}
				}
				if (!prefabLocalIds[entity.prefabInstanceRootId].insert(
					entity.prefabLocalId
				).second) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Prefab instance contains a duplicate local Entity ID"
					);
				}
			}
		}
		if (!entity.prefabLinks.empty()) {
			const ScenePrefabLink& active = entity.prefabLinks.front();
			if (
				active.assetId != entity.prefabAssetId ||
				active.sourcePath != entity.prefabSourcePath ||
				active.instanceRootId != entity.prefabInstanceRootId ||
				active.localId != entity.prefabLocalId
			) {
				addIssue(
					SceneValidationSeverity::Error,
					entity.id,
					"Active Prefab link does not match compatibility metadata"
				);
			}
			for (size_t linkIndex = 1;
				linkIndex < entity.prefabLinks.size();
				++linkIndex) {
				const ScenePrefabLink& link = entity.prefabLinks[linkIndex];
				if (
					(link.assetId.empty() && link.sourcePath.empty()) ||
					link.instanceRootId == 0 ||
					link.localId == 0
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Nested Prefab link metadata is incomplete"
					);
					continue;
				}
				const auto root = entitiesById.find(link.instanceRootId);
				if (root == entitiesById.end()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Nested Prefab instance root does not exist: " +
						std::to_string(link.instanceRootId)
					);
				} else {
					const auto rootLink = std::find_if(
						root->second->prefabLinks.begin(),
						root->second->prefabLinks.end(),
						[&link](const ScenePrefabLink& candidate) {
							return candidate.instanceRootId ==
								link.instanceRootId;
						}
					);
					const bool assetMatches =
						rootLink != root->second->prefabLinks.end() &&
						(!link.assetId.empty()
							? rootLink->assetId == link.assetId
							: rootLink->assetId.empty() &&
								rootLink->sourcePath == link.sourcePath);
					if (!assetMatches) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"Nested Prefab root metadata does not match"
						);
					}
				}
				if (!prefabLocalIds[link.instanceRootId].insert(
					link.localId
				).second) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Nested Prefab contains a duplicate local Entity ID"
					);
				}
			}
		}

		std::unordered_set<uint64_t> visited;
		const SceneEntity* current = &entity;
		while (current && current->parentId != 0) {
			if (!visited.insert(current->id).second) {
				if (reportedCycles.insert(entity.id).second) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Hierarchy contains a parent cycle"
					);
				}
				break;
			}
			const auto parent = entitiesById.find(current->parentId);
			current = parent == entitiesById.end() ? nullptr : parent->second;
		}

		bool activeInHierarchy = entity.active;
		std::unordered_set<uint64_t> activeVisited;
		const SceneEntity* activeParent = &entity;
		while (activeInHierarchy && activeParent && activeParent->parentId != 0) {
			if (!activeVisited.insert(activeParent->id).second) {
				activeInHierarchy = false;
				break;
			}
			const auto parent = entitiesById.find(activeParent->parentId);
			activeParent = parent == entitiesById.end() ? nullptr : parent->second;
			if (activeParent && !activeParent->active) {
				activeInHierarchy = false;
			}
		}
		for (const SceneComponent& component : entity.components) {
			auto validateEntityReference = [
				&entitiesById,
				&addIssue,
				&entity
			](uint64_t targetId, const char* label) {
				if (targetId != 0 && !entitiesById.contains(targetId)) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						std::string(label) + " Entity does not exist: " +
						std::to_string(targetId)
					);
				}
			};
			if (component.type == "PrefabAnimator") {
				for (const auto& clip : component.prefabAnimationClips) {
					for (size_t index = 0; index < clip.tracks.size(); ++index) {
						const auto& track = clip.tracks[index];
						if (track.property != "LocalRotation") continue;
						const std::string label = clip.name + " / Track " + std::to_string(index + 1);
						if (track.rotationInterpolation != "QuaternionSlerp" &&
							track.rotationInterpolation != "EulerContinuous") {
							// 将来/未知方式は保存値を残し、Runtimeの互換fallbackを通知する。
							addIssue(SceneValidationSeverity::Warning, entity.id,
								"Unknown rotation interpolation; using QuaternionSlerp: " +
								label + " / " + track.rotationInterpolation);
						} else if (track.rotationInterpolation == "EulerContinuous" && !track.keyframes.empty()) {
							Vector3 sampled{};
							// 実再生と同じ全キー検証を使い、警告だけで不正Eulerを正当化しない。
							if (!ScenePrefabAnimationEvaluator::TrySampleContinuousEuler(
								track, track.keyframes.front().time, sampled)) {
								addIssue(SceneValidationSeverity::Error, entity.id,
									"EulerContinuous requires finite values and nonnegative, strictly ordered key times: " + label);
							}
						}
					}
				}
			} else if (component.type == "AudioSource") {
				const bool validSpatialMode = IsValidAudioSpatialMode(
					component.audioSpatialMode
				);
				if (!validSpatialMode) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"AudioSource has an invalid spatialMode: " +
							component.audioSpatialMode
					);
				}
				if (
					component.audioSpatialMode != "TwoD" &&
					(
						component.audioMinimumDistance < 0.0f ||
						component.audioMinimumDistance >= component.audioMaximumDistance
					)
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"3D AudioSource minimumDistance must be >= 0 and below maximumDistance"
					);
				}
				if (
					component.audioSpatialMode == "ThreeDStereoArea" &&
					(!std::isfinite(component.audioStereoAreaWidth) ||
						component.audioStereoAreaWidth <= 0.0f)
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"ThreeD Stereo Area width must be finite and greater than zero"
					);
				}
				const bool streamCompatibilityInvalid =
					component.audioStreamFromDisk &&
					(component.audioSpatialMode != "TwoD" || component.audioBus != "BGM");
				if (streamCompatibilityInvalid) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Stream From Disk requires TwoD spatial mode and BGM Bus"
					);
				}
				if (
					validSpatialMode && component.audioSpatialMode != "TwoD" &&
					!component.audioClipPath.empty() && !streamCompatibilityInvalid
				) {
					if (Audio* audio = Audio::GetInstance();
						audio && audio->CanProbeAudioFileMetadata()) {
						AudioFileMetadata metadata{};
						std::string metadataError;
						if (!audio->TryGetAudioFileMetadata(
							component.audioClipPath.c_str(), metadata, &metadataError
						)) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"AudioSource clip metadata could not be read: " + metadataError
							);
						} else {
							const uint32_t requiredChannels =
								component.audioSpatialMode == "ThreeD" ? 1 : 2;
							if (metadata.channelCount != requiredChannels) {
								addIssue(
									SceneValidationSeverity::Warning,
									entity.id,
									DescribeAudioChannelMismatch(
										component.audioSpatialMode,
										metadata.channelCount
									)
								);
							}
						}
					}
				}
				if (component.audioPersistAcrossScenes && !component.audioStreamFromDisk) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Persist Across Scenes requires Stream From Disk"
					);
				}
				if (!std::isfinite(component.audioBgmFadeSeconds) || component.audioBgmFadeSeconds < 0.0f) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"BGM Fade Seconds must be finite and greater than or equal to zero"
					);
				}
				if (
					component.enabled && activeInHierarchy &&
					component.audioPlayOnStart && component.audioPersistAcrossScenes &&
					component.audioStreamFromDisk &&
					component.audioSpatialMode == "TwoD" && component.audioBus == "BGM"
				) {
					++persistentBgmPlayOnStartCount;
					if (firstPersistentBgmEntityId == 0) {
						firstPersistentBgmEntityId = entity.id;
					}
				}
			} else if (component.type == "AudioListener") {
				if (
					component.audioListenerMode != "ActiveCamera" &&
					component.audioListenerMode != "Entity" &&
					component.audioListenerMode != "Hybrid"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"AudioListener has an invalid mode: " +
							component.audioListenerMode
					);
				}
				if (component.enabled && activeInHierarchy) {
					++activeAudioListenerCount;
					if (firstAudioListenerEntityId == 0) {
						firstAudioListenerEntityId = entity.id;
					}
				}
			} else if (component.type == "TextRenderer") {
				if (
					!SceneUILayout::IsUIEntity(document, entity) && component.textRenderSpace != "ScreenOverlay" &&
					component.textRenderSpace != "Scene2D"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextRenderer has an unknown renderSpace: " +
							component.textRenderSpace
					);
				}
				if (component.textFontFamily.empty()) {
					addIssue(
						SceneValidationSeverity::Warning,
						entity.id,
						"TextRenderer fontFamily is empty; runtime fallback will be used"
					);
				}
				if (component.textFontSize < 1.0f || component.textFontSize > 512.0f) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextRenderer fontSize must be between 1 and 512"
					);
				}
				if (
					component.textFontWeight != "Regular" &&
					component.textFontWeight != "Bold"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextRenderer has an unknown fontWeight: " +
							component.textFontWeight
					);
				}
				if (
					component.textFontStyle != "Normal" &&
					component.textFontStyle != "Italic"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextRenderer has an unknown fontStyle: " +
							component.textFontStyle
					);
				}
				if (
					component.textHorizontalAlignment != "Left" &&
					component.textHorizontalAlignment != "Center" &&
					component.textHorizontalAlignment != "Right"
				) {
					addIssue(SceneValidationSeverity::Error, entity.id,
						"TextRenderer has an unknown horizontalAlignment");
				}
				if (
					component.textVerticalAlignment != "Top" &&
					component.textVerticalAlignment != "Center" &&
					component.textVerticalAlignment != "Bottom"
				) {
					addIssue(SceneValidationSeverity::Error, entity.id,
						"TextRenderer has an unknown verticalAlignment");
				}
				if (
					component.textWrapMode != "NoWrap" &&
					component.textWrapMode != "Word"
				) {
					addIssue(SceneValidationSeverity::Error, entity.id,
						"TextRenderer has an unknown wrapMode");
				}
				if (
					component.textOverflowMode != "Overflow" &&
					component.textOverflowMode != "Clip" &&
					component.textOverflowMode != "Ellipsis"
				) {
					addIssue(SceneValidationSeverity::Error, entity.id,
						"TextRenderer has an unknown overflowMode");
				}
				if (
					component.textLayoutSize.x < 0.0f ||
					component.textLayoutSize.y < 0.0f ||
					component.textLineSpacing < 0.1f ||
					component.textOpacity < 0.0f || component.textOpacity > 1.0f
				) {
					addIssue(SceneValidationSeverity::Error, entity.id,
					"TextRenderer contains an invalid layout or opacity value");
				}
				if (component.textHasPlacementProfiles && !SceneUILayout::IsUIEntity(document, entity)) {
					const auto validPlacement = [](const Text2DPlacement& placement, bool overlay) {
						return placement.scale.x > 0.0f && placement.scale.y > 0.0f &&
							(!overlay || (placement.viewportAnchor.x >= 0.0f && placement.viewportAnchor.x <= 1.0f &&
								placement.viewportAnchor.y >= 0.0f && placement.viewportAnchor.y <= 1.0f));
					};
					if (!validPlacement(component.textOverlayPlacement, true) ||
						!validPlacement(component.textScene2DPlacement, false)) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"TextRenderer contains an invalid 2D placement profile");
					}
				}
			} else if (component.type == "TextMotion") {
				const SceneComponent* textRenderer =
					SceneEntityQuery::FindEnabledComponent(entity, "TextRenderer");
				if (!textRenderer) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"TextMotion requires an enabled TextRenderer on the same Entity"
					);
				}
				std::unordered_set<std::string> clipIds;
				for (const SceneTextMotionClip& clip : component.textMotionClips) {
					if (clip.id.empty() || !clipIds.insert(clip.id).second) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"TextMotion contains an empty or duplicate clip Id");
					}
					if (clip.keyframes.size() < 2) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"TextMotion clip requires at least two keyframes: " + clip.id);
					}
					float previousTime = -1.0f;
					for (const SceneTextMotionKeyframe& keyframe : clip.keyframes) {
						const bool validEasing =
							keyframe.easingToNext == "Linear" ||
							keyframe.easingToNext == "EaseIn" ||
							keyframe.easingToNext == "EaseOut" ||
							keyframe.easingToNext == "EaseInOut" ||
							keyframe.easingToNext == "SmoothStep";
						if (!std::isfinite(keyframe.timeSeconds) ||
							!std::isfinite(keyframe.positionOffset.x) ||
							!std::isfinite(keyframe.positionOffset.y) ||
							!std::isfinite(keyframe.rotationOffset) ||
							!std::isfinite(keyframe.scaleMultiplier.x) ||
							!std::isfinite(keyframe.scaleMultiplier.y) ||
							!std::isfinite(keyframe.opacityMultiplier) ||
							keyframe.timeSeconds < 0.0f ||
							keyframe.timeSeconds <= previousTime ||
							keyframe.scaleMultiplier.x <= 0.0f ||
							keyframe.scaleMultiplier.y <= 0.0f ||
							keyframe.opacityMultiplier < 0.0f ||
							keyframe.opacityMultiplier > 1.0f || !validEasing) {
							addIssue(SceneValidationSeverity::Error, entity.id,
								"TextMotion clip contains an invalid keyframe: " + clip.id);
							break;
						}
						previousTime = keyframe.timeSeconds;
					}
					if (!clip.keyframes.empty() &&
						(clip.keyframes.front().timeSeconds != 0.0f ||
							clip.keyframes.back().timeSeconds <= 0.0f)) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"TextMotion clip must start at zero and end after zero: " +
								clip.id);
					}
				}
			} else if (component.type == "Light") {
				if (
					component.lightType != "Directional" &&
					component.lightType != "Point" &&
					component.lightType != "Spot"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"Light has an unknown lightType: " + component.lightType
					);
					continue;
				}
				if (!component.enabled || !activeInHierarchy) {
					continue;
				}
				if (component.lightType == "Directional") {
					++directionalLightCount;
					if (directionalLightCount > 1) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"Only the first active Directional Light is rendered"
						);
					}
				} else if (component.lightType == "Point") {
					++pointLightCount;
					if (pointLightCount > 16) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"Only the first 16 active Point Lights are rendered"
						);
					}
				} else {
					++spotLightCount;
					if (spotLightCount > 8) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"Only the first 8 active Spot Lights are rendered"
						);
					}
					if (component.lightCastsShadow) {
						++spotShadowCount;
						if (spotShadowCount > 4) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"Only the first 4 active Spot Light shadows are rendered"
							);
						}
					}
				}
			} else if (component.type == "MonitorRenderer") {
				validateEntityReference(
					component.monitorCameraEntityId,
					"Monitor camera"
				);
			} else if (component.type == "CameraSwitcher") {
				if (component.enabled && activeInHierarchy) {
					++cameraSwitcherCount;
					if (cameraSwitcherCount > 1) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"Only the first active CameraSwitcher is used"
						);
					}
				}
				std::unordered_set<uint64_t> registeredCameraIds;
				for (const SceneCameraSwitchEntry& entry :
					component.cameraSwitchEntries) {
					validateEntityReference(
						entry.cameraEntityId,
						"CameraSwitcher camera"
					);
					const SceneEntity* cameraEntity = entry.cameraEntityId != 0
						? document.FindEntity(entry.cameraEntityId)
						: nullptr;
					if (
						!entry.cameraEntityName.empty() &&
						(!cameraEntity ||
							cameraEntity->name != entry.cameraEntityName)
					) {
						cameraEntity = document.FindEntityByName(
							entry.cameraEntityName
						);
					}
					if (!cameraEntity) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"CameraSwitcher contains an unresolved camera"
						);
						continue;
					}
					const bool hasCamera = std::any_of(
						cameraEntity->components.begin(),
						cameraEntity->components.end(),
						[](const SceneComponent& candidate) {
							return candidate.enabled && candidate.type == "Camera";
						}
					);
					if (!hasCamera) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"CameraSwitcher target has no enabled Camera: " +
								cameraEntity->name
						);
					} else if (!registeredCameraIds.insert(cameraEntity->id).second) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"CameraSwitcher contains a duplicate camera: " +
								cameraEntity->name
						);
					}
				}
				if (component.cameraSwitchEntries.empty()) {
					addIssue(
						SceneValidationSeverity::Warning,
						entity.id,
						"CameraSwitcher has no registered cameras"
					);
				}
			} else if (component.type == "ThirdPersonCamera") {
				const bool hasCamera = std::any_of(
					entity.components.begin(),
					entity.components.end(),
					[](const SceneComponent& candidate) {
						return candidate.enabled && candidate.type == "Camera";
					}
				);
				if (!hasCamera) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"ThirdPersonCamera requires Camera on the same Entity"
					);
				}
				validateEntityReference(
					component.thirdPersonTargetEntityId,
					"ThirdPerson target"
				);
				if (
					component.thirdPersonTargetEntityId == 0 &&
					!component.thirdPersonTargetEntityName.empty() &&
					!document.FindEntityByName(
						component.thirdPersonTargetEntityName
					)
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"ThirdPerson target name cannot be resolved: " +
							component.thirdPersonTargetEntityName
					);
				}
				if (
					component.thirdPersonYawReference != "World" &&
					component.thirdPersonYawReference != "Target"
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"ThirdPersonCamera has an unknown yawReference: " +
							component.thirdPersonYawReference
					);
				}
			} else if (component.type == "AgentBehavior") {
				validateEntityReference(
					component.agentBoundsEntityId,
					"Agent bounds"
				);
				validateEntityReference(
					component.agentAttractorEntityId,
					"Agent attractor"
				);
			} else if (component.type == "EntityReference") {
				if (component.entityReferenceName.empty()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"EntityReference referenceName is empty"
					);
				}
				const SceneEntityReference& reference =
					component.entityReferenceTarget;
				if (reference.entityId == 0) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"EntityReference target Entity ID is empty"
					);
				} else if (
					reference.sceneId.empty() ||
					reference.sceneId == sceneId
				) {
					validateEntityReference(
						reference.entityId,
						"EntityReference target"
					);
				}
				if (
					reference.sceneId.empty() &&
					!reference.instanceKey.empty()
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"EntityReference instanceKey requires a target sceneId"
					);
				} else if (
					!reference.sceneId.empty() &&
					catalog &&
					!catalog->Find(reference.sceneId)
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"EntityReference target Scene is not registered: " +
						reference.sceneId
					);
				}
			} else if (component.type == "SceneTransition") {
				if (component.sceneTransitionTargetSceneId.empty()) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"SceneTransition targetSceneId is empty"
					);
				} else if (catalog &&
					!catalog->Find(component.sceneTransitionTargetSceneId)) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"SceneTransition target is not registered: " +
						component.sceneTransitionTargetSceneId
					);
				}
			} else if (component.type == "StatSet") {
				std::unordered_set<std::string> statIds;
				for (const SceneStatDefinition& stat : component.stats) {
					if (stat.id.empty()) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"StatSet contains an empty Stat Id"
						);
					} else if (!statIds.insert(stat.id).second) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"StatSet contains a duplicate Stat Id: " + stat.id
						);
					}
					if (stat.maxValue < stat.minValue) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"Stat max is below min: " + stat.id
						);
					}
				}
			} else if (component.type == "EventTrigger") {
				auto validateCameraEventTarget = [
					&addIssue,
					&document,
					&entity
				](
					uint64_t targetId,
					const std::string& targetName,
					const char* componentName,
					const char* label,
					bool checkPathPoints,
					bool allowInactiveTarget = false
				) {
					if (targetId == 0 && targetName.empty()) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							std::string(label) + " target is not set"
						);
						return;
					}
					const SceneEntity* target = targetId != 0
						? document.FindEntity(targetId)
						: nullptr;
					if (!target && !targetName.empty()) {
						target = document.FindEntityByName(targetName);
					}
					const SceneComponent* targetComponent = target
						? SceneEntityQuery::FindEnabledComponent(
							*target, componentName
						)
						: nullptr;
					if (
						!target || !targetComponent ||
						(!allowInactiveTarget &&
							!SceneEntityQuery::IsEntityActiveInHierarchy(document, *target))
					) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							std::string(label) +
								" target is unresolved, inactive, or disabled"
						);
						return;
					}
					if (!checkPathPoints) {
						return;
					}
					const bool hasPoint = std::any_of(
						document.GetEntities().begin(),
						document.GetEntities().end(),
						[&document, target](const SceneEntity& candidate) {
							return candidate.parentId == target->id &&
								SceneEntityQuery::IsEntityActiveInHierarchy(
									document, candidate
								) &&
								SceneEntityQuery::FindEnabledComponent(
									candidate, "CameraPathPoint"
								);
						}
					);
					if (!hasPoint) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"CameraPath Event target has no active CameraPathPoint"
						);
					}
					if (!targetComponent->cameraPathTargetCameraName.empty()) {
						const SceneEntity* targetCamera = document.FindEntityByName(
							targetComponent->cameraPathTargetCameraName
						);
						if (
							!targetCamera ||
							!SceneEntityQuery::IsEntityActiveInHierarchy(
								document, *targetCamera
							) ||
							!SceneEntityQuery::FindEnabledComponent(
								*targetCamera, "Camera"
							)
						) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"CameraPath Event target has an unresolved target Camera"
							);
						}
					}
				};
				auto resolveEventTarget = [&document](
					uint64_t targetId,
					const std::string& targetName
				) -> const SceneEntity* {
					const SceneEntity* target = targetId != 0
						? document.FindEntity(targetId)
						: nullptr;
					if (!target && !targetName.empty()) {
						target = document.FindEntityByName(targetName);
					}
					return target;
				};
				auto hasActiveParentHierarchy = [&document](const SceneEntity& target) {
					std::unordered_set<uint64_t> visited;
					const SceneEntity* current = &target;
					while (current->parentId != 0) {
						if (!visited.insert(current->id).second) {
							return false;
						}
						current = document.FindEntity(current->parentId);
						if (!current || !current->active) {
							return false;
						}
					}
					return true;
				};
				// 演出入口だけを診断する。従来のGameplay Clipには新制約を適用しない。
				auto validatePresentationTarget = [&](uint64_t targetId, const std::string& targetName,
					const std::string& clipName, bool needsClip) {
					const SceneEntity* owner = targetId != 0 ? document.FindEntity(targetId) : nullptr;
					if (targetId == 0 && !targetName.empty()) {
						size_t count = 0;
						for (const SceneEntity& candidate : document.GetEntities()) {
							if (candidate.name == targetName) { owner = &candidate; ++count; }
						}
						if (count != 1) owner = nullptr;
					}
					const SceneComponent* animator = owner
						? SceneEntityQuery::FindEnabledComponent(*owner, "PrefabAnimator") : nullptr;
					if ((targetId == 0 && targetName.empty()) || !animator) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"Presentation target is unset, unresolved, or has no enabled PrefabAnimator");
						return;
					}
					if (!needsClip) return;
					if (!hasActiveParentHierarchy(*owner)) {
						addIssue(SceneValidationSeverity::Error, entity.id, "Presentation target has an inactive or invalid parent");
					}
					auto checkOwnership = [&](const SceneEntity& target) {
						for (const char* type : { "PhysicsBody", "PlayerBehavior", "AttackSet", "StateMachine",
							"EnemyBehavior", "AgentBehavior", "Projectile", "BoneAttachment" }) {
							if (SceneEntityQuery::FindEnabledComponent(target, type)) {
								addIssue(SceneValidationSeverity::Error, entity.id,
									"Presentation Transform ownership conflicts with " + std::string(type) +
									" on Entity " + std::to_string(target.id));
							}
						}
					};
					checkOwnership(*owner);
					const ScenePrefabAnimationClip* selectedClip = nullptr;
					size_t clipCount = 0;
					for (const ScenePrefabAnimationClip& clip : animator->prefabAnimationClips) {
						if (clip.playOnStart) addIssue(SceneValidationSeverity::Error, entity.id,
							"Presentation Animator conflicts with Play On Start: " + clip.name);
						if (clip.name == clipName) { selectedClip = &clip; ++clipCount; }
					}
					if (clipName.empty() || clipCount != 1) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"Presentation requires an explicit, unique Clip name: " + clipName);
						return;
					}
					if (selectedClip->loop || !std::isfinite(selectedClip->duration) || selectedClip->duration <= 0.0f) {
						addIssue(SceneValidationSeverity::Error, entity.id,
							"Presentation Clip must be non-Loop with a positive finite duration: " + clipName);
					}
					std::unordered_map<uint64_t, std::unordered_set<std::string>> properties;
					for (const SceneAnimationTrack& track : selectedClip->tracks) {
						const SceneEntity* target = track.targetEntityId != 0
							? document.FindEntity(track.targetEntityId) : nullptr;
						if (track.targetEntityId == 0) {
							if (track.targetEntityName.empty()) target = owner;
							else {
								size_t count = 0;
								for (const SceneEntity& candidate : document.GetEntities()) {
									if (candidate.name == track.targetEntityName) { target = &candidate; ++count; }
								}
								if (count != 1) target = nullptr;
							}
						}
						if (!target || (target->id != owner->id && !document.IsDescendantOf(target->id, owner->id))) {
							addIssue(SceneValidationSeverity::Error, entity.id,
								"Presentation Track target must resolve uniquely to its owner or descendant");
							continue;
						}
						if (target != owner) checkOwnership(*target);
						const SceneEntity* current = target;
						std::unordered_set<uint64_t> visited;
						while (current && current->id != owner->id) {
							if (!current->active || !visited.insert(current->id).second) {
								addIssue(SceneValidationSeverity::Error, entity.id, "Presentation Track has an inactive descendant target or parent");
								break;
							}
							current = document.FindEntity(current->parentId);
						}
						if (track.property != "LocalPosition" && track.property != "LocalRotation" && track.property != "LocalScale") {
							addIssue(SceneValidationSeverity::Error, entity.id, "Presentation supports Transform Tracks only: " + track.property);
						}
						if (!properties[target->id].insert(track.property).second) {
							addIssue(SceneValidationSeverity::Error, entity.id, "Presentation contains a duplicate target/property Track: " + track.property);
						}
						for (const SceneAnimationKeyframe& key : track.keyframes) {
							if (!std::isfinite(key.time) || !std::isfinite(key.value.x) || !std::isfinite(key.value.y) ||
								!std::isfinite(key.value.z) || !std::isfinite(key.positionBulge.x) ||
								!std::isfinite(key.positionBulge.y) || !std::isfinite(key.positionBulge.z)) {
								addIssue(SceneValidationSeverity::Error, entity.id, "Presentation Track contains non-finite key values");
								break;
							}
						}
					}
				};
				std::unordered_set<uint64_t> onStartActivatedEntityIds;
				if (component.enabled && activeInHierarchy) {
					for (const SceneEventBinding& binding : component.eventBindings) {
						if (binding.triggerType != "OnStart") {
							continue;
						}
						for (const SceneEventAction& action : binding.actions) {
							if (action.type != "SetEntityActive" || !action.active) {
								continue;
							}
							const SceneEntity* target = resolveEventTarget(
								action.targetEntityId,
								action.targetEntityName
							);
							if (target && hasActiveParentHierarchy(*target)) {
								onStartActivatedEntityIds.insert(target->id);
							}
						}
					}
				}
				auto isActivatedOnStart = [
					&onStartActivatedEntityIds,
					&resolveEventTarget
				](uint64_t targetId, const std::string& targetName) {
					const SceneEntity* target = resolveEventTarget(targetId, targetName);
					return target && onStartActivatedEntityIds.contains(target->id);
				};
				for (const SceneEventBinding& binding : component.eventBindings) {
					const bool triggerUsesTarget =
						binding.triggerType == "OnStatReachedMin" ||
						binding.triggerType == "OnStatCompare" ||
						binding.triggerType == "OnPositionReached";
					if (triggerUsesTarget) {
						validateEntityReference(
							binding.targetEntityId,
							"Event target"
						);
					}
					if (
						binding.triggerType == "OnKeyPressed" &&
						ResolveSceneInputKey(binding.triggerKey) == 0
					) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"OnKeyPressed Event has an unsupported key: " +
								binding.triggerKey
						);
					}
					if (binding.triggerType == "OnCameraPathCompleted") {
						validateCameraEventTarget(
							binding.targetEntityId,
							binding.targetEntityName,
							"CameraPath",
							"OnCameraPathCompleted",
							true
						);
					}
					if (binding.triggerType == "OnAudioFinished") {
						validateCameraEventTarget(
							binding.targetEntityId,
							binding.targetEntityName,
							"AudioSource",
							"OnAudioFinished",
							false
						);
					}
					if (binding.triggerType == "OnTextMotionCompleted") {
						validateCameraEventTarget(
							binding.targetEntityId,
							binding.targetEntityName,
							"TextMotion",
							"OnTextMotionCompleted",
							false,
							isActivatedOnStart(
								binding.targetEntityId,
								binding.targetEntityName
							)
						);
						const SceneEntity* target = binding.targetEntityId != 0
							? document.FindEntity(binding.targetEntityId) : nullptr;
						if (!target && !binding.targetEntityName.empty()) {
							target = document.FindEntityByName(binding.targetEntityName);
						}
						const SceneComponent* motion = target
							? SceneEntityQuery::FindEnabledComponent(*target, "TextMotion")
							: nullptr;
						if (motion && !binding.textMotionClipId.empty() &&
							!std::any_of(
								motion->textMotionClips.begin(),
								motion->textMotionClips.end(),
								[&binding](const SceneTextMotionClip& clip) {
									return clip.id == binding.textMotionClipId;
								}
							)) {
							addIssue(SceneValidationSeverity::Error, entity.id,
								"OnTextMotionCompleted clip Id is unresolved: " +
									binding.textMotionClipId);
						}
					}
					if (binding.triggerType == "OnPresentationClipCompleted") {
						validatePresentationTarget(binding.targetEntityId, binding.targetEntityName,
							binding.presentationClipName, true);
					}
					for (size_t actionIndex = 0;
						actionIndex < binding.actions.size();
						++actionIndex) {
						const SceneEventAction& action = binding.actions[actionIndex];
						const bool actionUsesTarget =
							action.type == "ModifyStat" ||
							action.type == "SetEntityActive" ||
							action.type == "InstantiatePrefab" ||
							action.type == "ChangeState" ||
							action.type == "PlayAudio" ||
							action.type == "StopAudio" ||
							action.type == "PauseAudio" ||
							action.type == "ResumeAudio" ||
							action.type == "PlayTextMotion" ||
							action.type == "StopTextMotion" ||
								action.type == "ResetTextMotion" ||
								action.type == "PlayPresentationClip" ||
								action.type == "StopPresentationClip" ||
								action.type == "ResetPresentationClip";
						if (actionUsesTarget) {
							validateEntityReference(
								action.targetEntityId,
								"Event action target"
							);
						}
						if (
							(action.type == "PlayAudio" || action.type == "StopAudio" ||
								action.type == "PauseAudio" || action.type == "ResumeAudio")
						) {
							validateCameraEventTarget(
								action.targetEntityId, action.targetEntityName, "AudioSource",
								action.type.c_str(), false
							);
						}
						if (
							action.type == "PlayCameraPath" ||
							action.type == "StopCameraPath"
						) {
							validateCameraEventTarget(
								action.targetEntityId,
								action.targetEntityName,
								"CameraPath",
								action.type.c_str(),
								action.type == "PlayCameraPath"
							);
						} else if (action.type == "SelectCamera") {
							validateCameraEventTarget(
								action.targetEntityId,
								action.targetEntityName,
								"Camera",
								"SelectCamera",
								false
							);
						}
						if (
							action.type == "PlayTextMotion" ||
							action.type == "StopTextMotion" ||
							action.type == "ResetTextMotion"
						) {
							const bool activatedByEarlierOnStartAction =
								binding.triggerType == "OnStart" &&
								std::any_of(
									binding.actions.begin(),
									binding.actions.begin() + actionIndex,
									[&action](const SceneEventAction& priorAction) {
										if (priorAction.type != "SetEntityActive" ||
											!priorAction.active) {
											return false;
										}
										if (action.targetEntityId != 0) {
											return priorAction.targetEntityId == action.targetEntityId;
										}
										return !action.targetEntityName.empty() &&
											priorAction.targetEntityName == action.targetEntityName;
									}
								);
							validateCameraEventTarget(
								action.targetEntityId,
								action.targetEntityName,
								"TextMotion",
								action.type.c_str(),
								false,
								action.type == "PlayTextMotion" &&
									(binding.triggerType == "OnStart"
										? activatedByEarlierOnStartAction
										: isActivatedOnStart(
											action.targetEntityId,
											action.targetEntityName
										))
							);
							if (action.type == "PlayTextMotion") {
								const SceneEntity* target = action.targetEntityId != 0
									? document.FindEntity(action.targetEntityId) : nullptr;
								if (!target && !action.targetEntityName.empty()) {
									target = document.FindEntityByName(action.targetEntityName);
								}
								const SceneComponent* motion = target
									? SceneEntityQuery::FindEnabledComponent(*target, "TextMotion")
									: nullptr;
								const bool hasClip = motion && std::any_of(
									motion->textMotionClips.begin(),
									motion->textMotionClips.end(),
									[&action](const SceneTextMotionClip& clip) {
										return clip.id == action.textMotionClipId;
									}
								);
								if (!hasClip) {
									addIssue(SceneValidationSeverity::Error, entity.id,
										"PlayTextMotion clip Id is unresolved: " +
											action.textMotionClipId);
								}
							}
						}
						if (action.type == "PlayPresentationClip" || action.type == "StopPresentationClip" ||
							action.type == "ResetPresentationClip") {
							validatePresentationTarget(action.targetEntityId, action.targetEntityName,
								action.presentationClipName, action.type == "PlayPresentationClip");
						}
						if (
							action.type == "SetPostProcessProfile" ||
							action.type == "NextPostProcessProfile"
						) {
							const SceneEntity* manager =
								action.postProcessManagerEntityId != 0
								? document.FindEntity(action.postProcessManagerEntityId)
								: nullptr;
							if (!manager && !action.postProcessManagerEntityName.empty()) {
								manager = document.FindEntityByName(
									action.postProcessManagerEntityName
								);
							}
							const SceneComponent* managerComponent = nullptr;
							if (manager) {
								const auto componentIterator = std::find_if(
									manager->components.begin(),
									manager->components.end(),
									[](const SceneComponent& candidate) {
										return candidate.enabled &&
											candidate.type == "PostProcessProfileManager";
									}
								);
								if (componentIterator != manager->components.end()) {
									managerComponent = &*componentIterator;
								}
							}
							if (!managerComponent) {
								addIssue(
									SceneValidationSeverity::Error,
									entity.id,
									"Event PostProcess action has an unresolved or disabled manager"
								);
							} else if (action.type == "NextPostProcessProfile" &&
								managerComponent->postProcessProfiles.empty()) {
								addIssue(
									SceneValidationSeverity::Warning,
									entity.id,
									"NextPostProcessProfile manager has no profiles"
								);
							} else if (action.type == "SetPostProcessProfile") {
								const bool profileExists = std::any_of(
									managerComponent->postProcessProfiles.begin(),
									managerComponent->postProcessProfiles.end(),
									[&action](const ScenePostProcessProfile& profile) {
										return profile.id == action.postProcessProfileId;
									}
								);
								if (!profileExists) {
									addIssue(
										SceneValidationSeverity::Error,
										entity.id,
										"SetPostProcessProfile profile cannot be resolved: " +
											action.postProcessProfileId
									);
								}
							}
						}
						if (
							action.type == "ChangeState" &&
							action.stateName.empty()
						) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"ChangeState action has an empty state name"
							);
						}
						if (
							action.type == "SceneTransition" &&
							!action.transitionAssetPath.empty() &&
							(action.sceneId.empty() || !IsTransitionReferenceAvailable(action.transitionAssetPath))
						) {
							addIssue(SceneValidationSeverity::Error,entity.id,
								"Event SceneTransition asset is missing or invalid: "+action.transitionAssetPath);
						}
						if (
							action.type == "SceneTransition" &&
							!action.sceneId.empty() &&
							catalog &&
							!catalog->Find(action.sceneId)
						) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"Event SceneTransition target is not registered: " +
									action.sceneId
							);
						}
					}
				}
			} else if (component.type == "PostProcessProfileManager") {
				std::unordered_set<std::string> profileIds;
				for (const ScenePostProcessProfile& profile :
					component.postProcessProfiles) {
					if (profile.id.empty()) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"PostProcessProfileManager contains an empty Profile Id"
						);
					} else if (!profileIds.insert(profile.id).second) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"PostProcessProfileManager contains a duplicate Profile Id: " +
								profile.id
						);
					}
					if (profile.label.empty()) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"PostProcessProfileManager Profile has an empty label: " +
								profile.id
						);
					}
					const ScenePostProcessSettings& settings = profile.settings;
					if (
						settings.pixelationBlockSize < 1 ||
						settings.pixelationBlockSize > 64 ||
						settings.motionBlurStrength < 0.0f ||
						settings.motionBlurStrength > 1.0f ||
						settings.motionBlurSamples < 2 ||
						settings.motionBlurSamples > 32 ||
						settings.motionBlurMaxRadius < 0.0f ||
						settings.motionBlurMaxRadius > 64.0f ||
						settings.chromaticAberrationCenter.x < 0.0f ||
						settings.chromaticAberrationCenter.x > 1.0f ||
						settings.chromaticAberrationCenter.y < 0.0f ||
						settings.chromaticAberrationCenter.y > 1.0f ||
						settings.chromaticAberrationIntensity < 0.0f ||
						settings.chromaticAberrationFalloff <= 0.0f
					) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"Post Process settings are outside the supported range: " + profile.id
						);
					}
				}
			} else if (component.type == "AttackSet") {
				for (const SceneAttackDefinition& attack : component.attackDefinitions) {
					for (size_t windowIndex = 0;
						windowIndex < attack.hitWindows.size(); ++windowIndex) {
						const SceneAttackHitWindow& window =
							attack.hitWindows[windowIndex];
						if (window.payloadSource != "HitBox") {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"Attack '" + attack.name + "' Hit Window " +
									std::to_string(windowIndex + 1) +
									" uses WindowLegacy payload"
							);
							continue;
						}
						const SceneEntity* hitBox = window.hitBoxEntityId != 0
							? document.FindEntity(window.hitBoxEntityId)
							: nullptr;
						if (!hitBox && !window.hitBoxEntityName.empty()) {
							hitBox = document.FindEntityByName(window.hitBoxEntityName);
						}
						if (!hitBox) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"Attack '" + attack.name + "' Hit Window " +
									std::to_string(windowIndex + 1) +
									" has no resolvable Dedicated HitBox"
							);
							continue;
						}
						const bool hasEnabledHitBox = std::any_of(
							hitBox->components.begin(), hitBox->components.end(),
							[](const SceneComponent& candidate) {
								return candidate.enabled && candidate.type == "HitBox";
							}
						);
						const bool hasEnabledTrigger = std::any_of(
							hitBox->components.begin(), hitBox->components.end(),
							[](const SceneComponent& candidate) {
								return candidate.enabled && candidate.type == "OBBCollider" &&
									candidate.colliderIsTrigger;
							}
						);
						if (!hasEnabledHitBox || !hasEnabledTrigger) {
							addIssue(
								SceneValidationSeverity::Error,
								entity.id,
								"Attack '" + attack.name + "' Dedicated HitBox '" +
									hitBox->name +
									" requires enabled HitBox and Trigger Collider"
							);
						}
						if (hitBox->active) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"Attack '" + attack.name + "' Dedicated HitBox '" +
									hitBox->name + "' is saved active"
							);
						}
					}
					for (size_t leftIndex = 0;
						leftIndex < attack.hitWindows.size(); ++leftIndex) {
						const SceneAttackHitWindow& left = attack.hitWindows[leftIndex];
						if (left.payloadSource != "HitBox" || left.hitBoxEntityId == 0) {
							continue;
						}
						for (size_t rightIndex = leftIndex + 1;
							rightIndex < attack.hitWindows.size(); ++rightIndex) {
							const SceneAttackHitWindow& right = attack.hitWindows[rightIndex];
							if (
								right.payloadSource == "HitBox" &&
								left.hitBoxEntityId == right.hitBoxEntityId &&
								left.startTime < right.endTime &&
								right.startTime < left.endTime
							) {
								addIssue(
									SceneValidationSeverity::Warning,
									entity.id,
									"Attack '" + attack.name + "' has overlapping Dedicated HitBox Windows " +
										std::to_string(leftIndex + 1) + " and " +
										std::to_string(rightIndex + 1)
								);
							}
						}
					}
				}
			} else if (component.type == "StateMachine") {
				std::unordered_set<std::string> stateNames;
				for (const SceneStateDefinition& state :
					component.stateMachineStates) {
					if (state.name.empty()) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"StateMachine contains an empty state name"
						);
					} else if (!stateNames.insert(state.name).second) {
						addIssue(
							SceneValidationSeverity::Error,
							entity.id,
							"StateMachine contains a duplicate state: " + state.name
						);
					}
					if (state.actionId.empty()) {
						addIssue(
							SceneValidationSeverity::Warning,
							entity.id,
							"State has an empty Action Id: " + state.name
						);
					}
					std::unordered_set<std::string> parameterNames;
					for (const SceneStateParameter& parameter : state.parameters) {
						if (!parameterNames.insert(parameter.name).second) {
							addIssue(
								SceneValidationSeverity::Warning,
								entity.id,
								"State contains a duplicate parameter: " +
									state.name + "." + parameter.name
							);
						}
						validateEntityReference(
							parameter.entityId,
							"State parameter"
						);
					}
				}
				if (
					component.stateMachineStates.empty() ||
					!stateNames.contains(component.stateMachineInitialState)
				) {
					addIssue(
						SceneValidationSeverity::Error,
						entity.id,
						"StateMachine initial state cannot be resolved"
					);
				}
			} else if (
				component.type == "HitBox" ||
				component.type == "HurtBox"
			) {
				const SceneComponent* collider = nullptr;
				for (const SceneComponent& candidate : entity.components) {
					if (candidate.type == "OBBCollider" && candidate.enabled) {
						collider = &candidate;
						break;
					}
				}
				if (!collider || !collider->colliderIsTrigger) {
					addIssue(
						SceneValidationSeverity::Warning,
						entity.id,
						component.type +
							" requires an enabled Trigger Collider on the same Entity"
					);
				}
				if (component.type == "HitBox") {
					validateEntityReference(
						component.hitBoxOwnerEntityId,
						"HitBox owner"
					);
				} else {
					validateEntityReference(
						component.hurtBoxStatsEntityId,
						"HurtBox stats owner"
					);
				}
			} else if (component.type == "BoneAttachment") {
				validateEntityReference(
					component.boneAttachmentTargetEntityId,
					"BoneAttachment target"
				);
				if (component.boneAttachmentJointName.empty()) {
					addIssue(
						SceneValidationSeverity::Warning,
						entity.id,
						"BoneAttachment jointName is empty"
					);
				}
				if (
					component.boneAttachmentAlignmentMode != "ManualOffset" &&
					component.boneAttachmentAlignmentMode != "MatchSourceBone"
				) {
					addIssue(
						SceneValidationSeverity::Warning,
						entity.id,
						"BoneAttachment alignmentMode is invalid"
					);
				} else if (
					component.boneAttachmentAlignmentMode == "MatchSourceBone" &&
					component.boneAttachmentSourceJointName.empty()
				) {
					addIssue(
						SceneValidationSeverity::Warning,
						entity.id,
						"BoneAttachment sourceJointName is empty"
					);
				}
			} else if (component.type == "EnemyBehavior") {
				validateEntityReference(
					component.enemyTargetEntityId,
					"Enemy target"
				);
				validateEntityReference(
					component.enemyAttackHitBoxEntityId,
					"Enemy attack HitBox"
				);
			} else if (component.type == "Projectile") {
				validateEntityReference(
					component.projectileHomingTargetEntityId,
					"Projectile homing target"
				);
			}
		}
	}
	if (activeAudioListenerCount > 1) {
		addIssue(
			SceneValidationSeverity::Error,
			firstAudioListenerEntityId,
			"Scene contains multiple active AudioListener components"
		);
	}
	if (persistentBgmPlayOnStartCount > 1) {
		addIssue(
			SceneValidationSeverity::Error,
			firstPersistentBgmEntityId,
			"Scene contains multiple Play On Start Persistent BGM AudioSources"
		);
	}

	for (size_t index = firstIssueIndex; index < issues.size(); ++index) {
		if (issues[index].severity == SceneValidationSeverity::Error) {
			return false;
		}
	}
	return true;
}

bool SceneValidator::ValidateCatalog(
	const SceneCatalog& catalog,
	std::vector<SceneValidationIssue>& issues
) {
	issues.clear();
	for (const SceneDescriptor& descriptor : catalog.GetScenes()) {
		SceneDocument document;
		if (!document.Load(descriptor.filePath)) {
			issues.push_back({
				SceneValidationSeverity::Error,
				descriptor.id,
				descriptor.filePath,
				0,
				"Scene could not be loaded: " + document.GetLastLoadError()
			});
			continue;
		}
		if (!document.GetLastLoadError().empty()) {
			issues.push_back({
				SceneValidationSeverity::Warning,
				descriptor.id,
				descriptor.filePath,
				0,
				document.GetLastLoadError()
			});
		} else if (document.IsDirty()) {
			issues.push_back({
				SceneValidationSeverity::Warning,
				descriptor.id,
				descriptor.filePath,
				0,
				"Scene JSON was migrated in memory; save it in the Editor"
			});
		}
		ValidateDocument(
			document,
			&catalog,
			descriptor.id,
			descriptor.filePath,
			issues
		);
		for (const SceneEntity& entity : document.GetEntities()) {
			for (const SceneComponent& component : entity.components) {
				if (
					component.type != "EntityReference" ||
					component.entityReferenceTarget.entityId == 0 ||
					component.entityReferenceTarget.sceneId.empty() ||
					component.entityReferenceTarget.sceneId == descriptor.id
				) {
					continue;
				}
				const SceneDescriptor* targetDescriptor = catalog.Find(
					component.entityReferenceTarget.sceneId
				);
				if (!targetDescriptor) {
					continue;
				}
				SceneDocument targetDocument;
				if (
					targetDocument.Load(targetDescriptor->filePath) &&
					!targetDocument.FindEntity(
						component.entityReferenceTarget.entityId
					)
				) {
					issues.push_back({
						SceneValidationSeverity::Error,
						descriptor.id,
						descriptor.filePath,
						entity.id,
						"EntityReference target Entity does not exist in Scene " +
							targetDescriptor->id + ": " +
							std::to_string(
								component.entityReferenceTarget.entityId
							)
					});
				}
			}
		}
	}
	return !HasErrors(issues);
}

std::string SceneValidator::FormatIssues(
	const std::vector<SceneValidationIssue>& issues,
	size_t maxIssueCount
) {
	std::ostringstream output;
	const size_t count = (std::min)(issues.size(), maxIssueCount);
	for (size_t index = 0; index < count; ++index) {
		const SceneValidationIssue& issue = issues[index];
		if (index != 0) {
			output << '\n';
		}
		output << (issue.severity == SceneValidationSeverity::Error
			? "Error"
			: "Warning");
		if (!issue.sceneId.empty()) {
			output << " [" << issue.sceneId << "]";
		}
		if (issue.entityId != 0) {
			output << " Entity " << issue.entityId;
		}
		output << ": " << issue.message;
	}
	if (issues.size() > count) {
		output << '\n' << "... and " << (issues.size() - count) << " more";
	}
	return output.str();
}
