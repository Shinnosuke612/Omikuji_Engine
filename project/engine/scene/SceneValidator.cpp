// 役割: Entity ID、Hierarchy、Entity参照、Scene遷移先の整合性を検証する。
#include "SceneValidator.h"

#include "SceneCatalog.h"
#include "SceneDocument.h"
#include "SceneEntityQuery.h"
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
					component.textRenderSpace != "ScreenOverlay" &&
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
				if (component.textHasPlacementProfiles) {
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
