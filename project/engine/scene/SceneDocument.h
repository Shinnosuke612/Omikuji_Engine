// 役割: Entity、Component、Hierarchy、チーム設定を保存可能なシーンデータとして所有する。
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "SceneSettings.h"
#include "SceneMovementBounds.h"
#include "SceneReplayPresentation.h"
#include "SceneEntityReference.h"
#include "../math/Transform.h"
#include "../math/Vector2.h"
#include "../math/Vector4.h"

// 地上敵の位置取り設定。距離はXZ上の移動Collider外周からの余裕、速度はScene単位/秒。
struct SceneEnemyPositioningSettings {
	float distance = 4.0f, tolerance = 0.5f, spacing = 1.0f;
	float retreatRatio = 0.6f, orbitRatio = 0.5f, acceleration = 8.0f;
	bool variation = true, randomizeOnPlay = true;
	float distanceVariation = 1.0f, spacingVariation = 0.25f, speedVariation = 0.15f;
	uint32_t seed = 1;
	// 設定値だけを検証する。Collider/参照先の存在は呼出側で確認する。
	bool IsValid(float baseSpeed) const;
};

// 新方式の攻撃段階。保存設定の対象段階bitはAim/Locked/Chargeだけを使う。
enum class SceneEnemyChargePhase { Idle, Aim, Locked, Charge, Recovery, Returning, Interrupted };
enum class SceneEnemyChargeEnd { None, Distance, Wall, Enemy, Player, Interrupted, Invalid, TargetLost };
enum class SceneEnemyInterruptConditionType { SingleHpDamage, AccumulatedHpDamage, HitCount, HorizontalKnockback, VerticalKnockback, PoiseDepleted };
struct SceneEnemyInterruptCondition {
	SceneEnemyInterruptConditionType type = SceneEnemyInterruptConditionType::HitCount;
	float threshold = 3.0f;
};
struct SceneEnemyInterruptRule {
	bool all = false;
	bool validCombination = true; // 未知の保存enumをAnyへ黙って変換しないための読込状態。
	std::vector<SceneEnemyInterruptCondition> conditions{{}};
	bool IsValid() const;
};
struct SceneEnemyInterruptSettings {
	bool enabled = false;
	uint8_t phases = 7;
	bool countWindows = false;
	float idleResetSeconds = 0.0f;
	SceneEnemyInterruptRule common;
	std::array<bool, 3> overrideEnabled{};
	std::array<SceneEnemyInterruptRule, 3> overrides;
	bool IsValid() const;
};
// 時間は秒、移動はworld Scene単位、姿勢角は度。姿勢は見た目の子のlocal差分。
struct SceneEnemyChargeSettings {
	float startDistance = 0.0f; // 0は最大突進距離から自動。正数は接触までの移動量の開始上限。
	std::string movementEasing = "Linear"; // 未知名も保存して保持する。時間/距離の意味は曲線で変えない。
	bool poseEnabled = true; // 旧保存値は姿勢必須。Editorでの新規追加だけOFFにする。
	float aimSeconds = 0.4f, lockedSeconds = 0.25f, speed = 10.0f, distance = 8.0f;
	float recoverySeconds = 0.5f, returnSeconds = 0.25f, cooldownSeconds = 1.0f, firstWaitSeconds = 0.5f;
	float aimVariation = 0.1f, lockedVariation = 0.05f, cooldownVariation = 0.3f, firstWaitVariation = 0.3f;
	uint64_t poseEntityId = 0;
	float backAngle = 15.0f, forwardAngle = 25.0f, leanSeconds = 0.1f;
	Vector3 axis{1.0f, 0.0f, 0.0f}, pivot{}, aimOffset{}, chargeOffset{};
	bool IsValid(bool variation) const;
};

// 保存参照の方式。Unknownは原文を保持し、既知方式へ暗黙変換しない。
enum class SceneEnemyTargetMode { LegacyReference, AutoPlayer, Entity, Name, Unknown };
enum class SceneEnemyTargetIssue { None, Unassigned, Missing, Ambiguous, Inactive, Self, UnsupportedPlayer, UnknownMode, Dead };
struct SceneEnemyTargetResolution {
	uint64_t entityId = 0;
	SceneEnemyTargetIssue issue = SceneEnemyTargetIssue::None;
};
enum class SceneEnemyAttackWaitReason {
	Configuration, Target, Range, Suppressed, Positioning, Cooldown, Attacking, Ready,
	ProbePending, StartDistance, FrontEnemy, FrontObstacle, HorizontalPath, CollisionMask,
	OwnerShape, TargetShape, InvalidShape
};
// 攻撃判断を所有するEnemyから、速度を選ぶAgentへ渡す非保存の目的。
enum class SceneEnemyPositioningIntent { KeepSpacing, ApproachForAttack, FindClearLane };

// Editorへ値コピーする非保存状態。Scene/Object/Runtimeへの可変参照を渡さない。
enum class SceneEnemyPositioningPhase { Waiting, Approaching, Retreating, Holding, Orbiting, Blocked, Suppressed, Invalid };
struct SceneEnemyPositioningStatus {
	uint64_t resolvedTargetEntityId = 0;
	std::string targetDisplayName;
	SceneEnemyTargetIssue targetIssue = SceneEnemyTargetIssue::Unassigned;
	SceneEnemyAttackWaitReason attackWaitReason = SceneEnemyAttackWaitReason::Target;
	std::string configurationIssue;
	bool positioningAvailable = false;
	SceneEnemyPositioningPhase phase = SceneEnemyPositioningPhase::Waiting;
	float distance = 0.0f, spacing = 0.0f, speed = 0.0f;
	int orbitSide = 1;
	uint64_t generation = 0;
	SceneEnemyChargePhase chargePhase = SceneEnemyChargePhase::Idle;
	SceneEnemyChargeEnd chargeEnd = SceneEnemyChargeEnd::None;
	float accumulatedDamage = 0.0f;
	uint32_t hitCount = 0;
	uint8_t achievedConditions = 0;
	bool attackReady = false; // 構成上の準備。今の開始可否はcanStartAttackで区別する。
	bool canStartAttack = false, contactDistanceValid = false, firstAttackWait = true;
	float contactDistance = 0.0f, startDistanceLimit = 0.0f, attackWaitSeconds = 0.0f;
	uint64_t blockerEntityId = 0;
	std::string blockerDisplayName;
	SceneEnemyPositioningIntent positioningIntent = SceneEnemyPositioningIntent::KeepSpacing;
	bool noSafePositioningCandidate = false;
};

struct SceneTeamSettings {
	std::string name = "Team";
	bool agentBehaviorOverride = false;
	std::string agentGroupName;
	float agentMinSpeed = 1.0f;
	float agentMaxSpeed = 3.0f;
	float agentTurnSpeed = 2.5f;
	float agentWanderStrength = 0.8f;
	float agentWanderChangeInterval = 4.0f;
	float agentWanderDirectionRange = 1.1f;
	float agentWanderVerticalRange = 0.18f;
	bool agentRandomizeSeedOnPlay = true;
	int agentRandomSeed = 1;
	bool agentUseLeaderStartPosition = false;
	Vector3 agentLeaderStartPosition{};
	float agentFlockDecisionInterval = 0.25f;
	float agentFlockAcceleration = 4.0f;
	float agentFlockTurnRate = 1.5f;
	float agentMemberCenterFollow = 1.5f;
	float agentMemberJitterStrength = 0.35f;
	float agentMemberJitterFrequency = 0.9f;
	float agentMemberJitterUpdateInterval = 0.5f;
	float agentMemberJitterFollowSpeed = 2.0f;
	float agentMemberSpeedVariation = 0.15f;
	float agentMemberLeashDistance = 4.0f;
	float agentMemberLeashStrength = 1.5f;
	float agentMemberCatchupSpeed = 2.0f;
	float agentMemberSeparationUpdateInterval = 0.1f;
	float agentMemberSeparationBlend = 0.5f;
	bool agentUseTeamHeading = false;
	bool agentTeamHeadingFromAverage = true;
	Vector3 agentTeamHeadingDirection = { 0.0f, 0.0f, 1.0f };
	float agentTeamHeadingWeight = 0.75f;
	float agentTeamHeadingFollowSpeed = 2.5f;
	bool agentUseTeamRotation = false;
	float agentTeamRotationWeight = 0.6f;
	float agentTeamRotationFollowSpeed = 4.0f;
	bool agentAlignForwardToVelocity = true;
	std::string agentForwardAxis = "+Z";
	bool agentRotateAxisX = true;
	bool agentRotateAxisY = true;
	bool agentRotateAxisZ = false;
	float agentRotationFollowSpeed = 12.0f;
	float agentPitchFromVerticalVelocity = 1.0f;
	float agentBankingStrength = 0.0f;
	bool agentSchooling = false;
	float agentSchoolingUpdateInterval = 0.0f;
	float agentSchoolingUpdateJitter = 0.0f;
	int agentNeighborLimit = 0;
	float agentSchoolingBlend = 1.0f;
	float agentSeparationRadius = 1.2f;
	float agentAlignmentRadius = 4.0f;
	float agentCohesionRadius = 5.0f;
	float agentSeparationWeight = 1.8f;
	float agentAlignmentWeight = 0.8f;
	float agentCohesionWeight = 0.9f;
	Vector4 agentVisualColor = { 0.25f, 0.75f, 1.0f, 1.0f };
	bool agentEnableLighting = true;
};

// MeshRendererが持つ、モデル標準材質へのEntity単位の上書き設定。
struct SceneMeshMaterialOverride {
	std::string materialName;
	bool enabled = false;
	bool colorOverrideEnabled = false;
	Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
	std::string texturePath;
};

struct SceneStatDefinition {
	std::string id = "stat";
	std::string displayName = "Stat";
	float minValue = 0.0f;
	float maxValue = 100.0f;
	float initialValue = 100.0f;
};

// 数値Statとは別型。保存するのは初期値だけで、現在値はStatSystemが所有する。
struct SceneFlagDefinition {
	std::string id;
	std::string displayName;
	bool initialValue = false;
};

struct SceneEventAction {
	std::string type = "SetEntityActive";
	uint64_t targetEntityId = 0;
	std::string targetEntityName;
	std::string statId;
	std::string statOperation = "Add";
	std::string flagId;
	bool flagValue = false;
	float value = 0.0f;
	bool active = true;
	std::string sceneId;
	std::string transitionAssetPath; // 空は既存の即時SceneTransition。
	std::string prefabPath;
	bool prefabParentToTarget = false;
	bool prefabUseTargetTransform = true;
	std::string stateName;
	uint64_t postProcessManagerEntityId = 0;
	std::string postProcessManagerEntityName;
	std::string postProcessProfileId;
	std::string textMotionClipId;
	// 演出ActionはClip名を明示する。旧JSONの欠落値は空のまま扱う。
	std::string presentationClipName;
};

struct SceneEventBinding {
	// UIは独立した制御段階で評価する。旧JSONはGameplayのまま扱う。
	std::string executionDomain = "Gameplay";
	std::string uiInputScope = "WhenNoMenu";
	std::string triggerType = "OnStart";
	std::string triggerKey;
	// OnKeyPressedのOR候補。先頭はtriggerKeyに保持し、旧データは追加なしとして読む。
	std::vector<std::string> additionalTriggerKeys;
	uint64_t targetEntityId = 0;
	std::string targetEntityName;
	std::string statId = "hp";
	std::string statComparison = "LessOrEqual";
	float statValue = 0.0f;
	std::string flagId;
	bool flagValue = true;
	Vector3 targetPosition{};
	float radius = 1.0f;
	bool triggerOnce = true;
	float cooldown = 0.0f;
	std::string textMotionClipId;
	std::string presentationClipName;
	std::vector<SceneEventAction> actions;
};

// 完全なPost Process設定をManager Entity単位で切り替えるAuthoring Profile。
// Runtime中の選択状態は保存せず、SceneEventSystemから要求として渡す。
enum class ScenePostProcessAutomationParameter {
	DissolveThreshold
};

enum class ScenePostProcessAutomationPlayback {
	OneShot
};

enum class ScenePostProcessAutomationEasing {
	Linear
};

struct ScenePostProcessAutomation {
	ScenePostProcessAutomationParameter parameter =
		ScenePostProcessAutomationParameter::DissolveThreshold;
	float startValue = 0.0f;
	float endValue = 1.0f;
	float duration = 2.0f;
	ScenePostProcessAutomationPlayback playback =
		ScenePostProcessAutomationPlayback::OneShot;
	ScenePostProcessAutomationEasing easing =
		ScenePostProcessAutomationEasing::Linear;
};

struct ScenePostProcessProfile {
	std::string id = "Profile1";
	std::string label = "Profile 1";
	ScenePostProcessSettings settings{};
	std::vector<ScenePostProcessAutomation> automations;
};

struct SceneAnimationKeyframe {
	float time = 0.0f;
	Vector3 value{};
	// Empty inherits the owning Track setting. The start Key owns the segment
	// from itself to the next Key.
	std::string easingToNext;
	// LocalPosition-only midpoint offset. Zero keeps the segment straight.
	Vector3 positionBulge{};
};

// 攻撃開始からの秒とColliderローカル値。Windowだけが所有し、Pose Trackとは独立。
struct SceneAttackHitGeometryKey {
	float time = 0;
	Vector3 offset{}, halfSize{1, 1, 1};
	float radius = 1;
	std::string interpolation = "Linear";
};
struct SceneAttackHitGeometryTrack {
	std::string shape;
	std::vector<SceneAttackHitGeometryKey> keys;
};
struct SceneAttackHitWindow {
	float startTime = 0.15f;
	float endTime = 0.35f;
	uint64_t hitBoxEntityId = 0;
	std::string hitBoxEntityName;
	// HitBoxは新規専用EntityのPayloadを使い、WindowLegacyは旧Window Payloadを適用する。
	std::string payloadSource = "HitBox";
	float damage = 10.0f;
	float poiseDamage = 0.0f;
	float knockback = 0.0f;
	// 水平Knockbackとは独立した、被弾時に一度だけ適用する上向き初速。
	float verticalKnockback = 0.0f;
	// 有効Window中だけBox ColliderのHalf Sizeを置き換えるAuthoring値。
	bool overrideHitBoxHalfSize = false;
	Vector3 hitBoxHalfSize = { 1.0f, 1.0f, 1.0f };
	float hitStopDuration = 0.0f;
	std::string reactionTag = "Light";
	std::string knockbackDirectionMode = "RadialFromAttacker";
	Vector3 knockbackLocalDirection = { 0.0f, 0.0f, 1.0f };
	std::string hitPolicy = "OncePerActivation";
	float targetCooldown = 0.15f;
	SceneAttackHitGeometryTrack geometryTrack;
};

// AttackRunner時刻から一発Particleを発火するためのAuthoringデータ。
struct SceneAttackEffectEvent {
	float time = 0.15f;
	std::string particleEffectPath;
	uint64_t spawnEntityId = 0;
	std::string spawnEntityName;
	Vector3 localOffset{};
	// 空でない場合、Spawn Entity下方のStatic Colliderへ設置する短命Prefab。
	std::string groundPrefabPath;
	float groundProbeDistance = 6.0f;
	float groundPrefabLifetime = 1.2f;
	// None / Prefab / ProceduralCrack。旧Prefab Pathだけのデータは読込時にPrefabへ正規化する。
	std::string groundEffectType;
	float groundCrackRadius = 3.2f;
	uint32_t groundCrackPrimaryBranchCount = 10;
	uint32_t groundCrackSegmentsPerBranch = 6;
	float groundCrackBranchProbability = 0.25f;
	float groundCrackWidth = 0.06f;
	float groundCrackLifetime = 1.2f;
	float groundCrackSurfaceOffset = 0.03f;
};

// AttackDefinitionはStateMachineから再生する攻撃時系列の共有データ。
struct SceneAttackDefinition {
	std::string name = "Attack";
	std::string animation;
	uint64_t animationTargetEntityId = 0;
	std::string animationTargetEntityName;
	uint64_t hitBoxEntityId = 0;
	std::string hitBoxEntityName;
	float windup = 0.15f;
	float activeTime = 0.2f;
	float recovery = 0.35f;
	float forwardDistance = 0.0f;
	float sideDistance = 0.0f;
	std::string motionEasing = "SmoothStep";
	// FixedAtStartは既存互換。InputDirectionはRuntimeSceneがPlayer入力から渡すXZ方向を使う。
	std::string facingMode = "FixedAtStart";
	uint64_t facingTargetEntityId = 0;
	std::string facingTargetEntityName;
	float facingRotateAngle = 0.0f;
	bool loopEnabled = false;
	int loopMaxCount = 0;
	float loopSafetyTimeout = 0.0f;
	std::vector<SceneAttackHitWindow> hitWindows;
	std::vector<SceneAttackEffectEvent> effectEvents;
};

struct SceneAnimationTrack {
	uint64_t targetEntityId = 0;
	std::string targetEntityName;
	std::string property = "LocalPosition";
	std::string easing = "SmoothStep";
	std::vector<SceneAnimationKeyframe> keyframes;
	// LocalRotationの補間方式。valueは周回を含むEuler radianのまま保存する。
	// 旧JSON/新Trackは従来Slerp。未知値も保存し、Validatorで通知する。
	std::string rotationInterpolation = "QuaternionSlerp";
};

struct ScenePrefabAnimationClip {
	std::string name = "Animation";
	float duration = 1.0f;
	bool loop = false;
	bool playOnStart = true;
	std::vector<SceneAnimationTrack> tracks;
};

struct SceneCameraSwitchEntry {
	uint64_t cameraEntityId = 0;
	std::string cameraEntityName;
};

struct SceneStateParameter {
	std::string name = "Parameter";
	std::string type = "Float";
	float floatValue = 0.0f;
	int intValue = 0;
	bool boolValue = false;
	std::string stringValue;
	uint64_t entityId = 0;
	std::string entityName;
};

struct SceneStateDefinition {
	std::string name = "State";
	std::string actionId = "Builtin.Idle";
	std::vector<SceneStateParameter> parameters;
};

struct Text2DPlacement {
	Vector2 position = { 0.0f, 0.0f };
	float rotation = 0.0f;
	Vector2 scale = { 1.0f, 1.0f };
	Vector2 pivot = { 0.0f, 0.0f };
	Vector2 viewportAnchor = { 0.0f, 0.0f };
	int sortingOrder = 0;
	bool clipEnabled = false;
};

struct SceneTextMotionKeyframe {
	float timeSeconds = 0.0f;
	Vector2 positionOffset = { 0.0f, 0.0f };
	float rotationOffset = 0.0f;
	Vector2 scaleMultiplier = { 1.0f, 1.0f };
	float opacityMultiplier = 1.0f;
	std::string easingToNext = "SmoothStep";
};

struct SceneTextMotionClip {
	std::string id;
	bool holdFinalPose = false;
	std::vector<SceneTextMotionKeyframe> keyframes;
};

struct SceneGameFlowWave {
	uint64_t spawnerEntityId = 0;
	int count = 1;
};

struct SceneGameFlowPhase {
	std::string id;
	std::string label;
	std::vector<SceneGameFlowWave> waves;
};

struct SceneComponent {
	SceneComponent() = default;
	SceneComponent(const char* componentType) : type(componentType ? componentType : "") {}
	SceneComponent(std::string componentType, bool componentEnabled = true)
		: type(std::move(componentType)), enabled(componentEnabled) {}

	// Prefab Property OverrideがComponentの並び順ではなく安定IDで対応付ける。
	uint64_t localId = 0;
	std::string type;
	bool enabled = true;
	std::string modelPath;
	std::string meshCullMode = "Back";
	bool meshEnvironmentReflectionOverride = false;
	float meshEnvironmentReflectionIntensity = 0.3f;
	std::vector<SceneMeshMaterialOverride> meshMaterialOverrides;
	std::string texturePath;
	bool environmentSkyboxEnabled = true;
	std::string environmentSkyboxPath;
	float environmentSkyboxIntensity = 1.0f;
	float environmentReflectionIntensity = 0.3f;
	Vector2 spriteSize = { 100.0f, 100.0f };
	Vector2 spriteAnchor = { 0.5f, 0.5f };
	Vector4 spriteColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	bool spriteFlipX = false;
	bool spriteFlipY = false;
	// UI配置は旧Transform/Text配置へ同期せず、Canvasの制作座標で保存する。
	std::array<uint32_t, 2> uiReferenceSize = { 1280, 720 };
	std::string uiRenderSpace = "ScreenOverlay";
	int uiCanvasSortingOrder = 0;
	Vector2 uiPosition = { 0.0f, 0.0f };
	Vector2 uiSize = { 100.0f, 100.0f };
	Vector2 uiAnchor = { 0.0f, 0.0f };
	Vector2 uiPivot = { 0.0f, 0.0f };
	float uiRotation = 0.0f;
	Vector2 uiScale = { 1.0f, 1.0f };
	int uiSortingOrder = 0;
	std::string textValue = "Text";
	std::string textRenderSpace = "ScreenOverlay";
	std::string textFontFamily = "Yu Gothic UI";
	float textFontSize = 32.0f;
	std::string textFontWeight = "Regular";
	std::string textFontStyle = "Normal";
	Vector4 textColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	float textOpacity = 1.0f;
	std::string textHorizontalAlignment = "Left";
	std::string textVerticalAlignment = "Top";
	std::string textWrapMode = "NoWrap";
	std::string textOverflowMode = "Overflow";
	Vector2 textLayoutSize = { 0.0f, 0.0f };
	float textCharacterSpacing = 0.0f;
	float textLineSpacing = 1.0f;
	bool textOutlineEnabled = false;
	Vector4 textOutlineColor = { 0.0f, 0.0f, 0.0f, 1.0f };
	float textOutlineWidth = 2.0f;
	bool textShadowEnabled = false;
	Vector4 textShadowColor = { 0.0f, 0.0f, 0.0f, 0.5f };
	Vector2 textShadowOffset = { 2.0f, 2.0f };
	Vector2 textViewportAnchor = { 0.0f, 0.0f };
	Vector2 textPivot = { 0.0f, 0.0f };
	int textSortingOrder = 0;
	bool textClipEnabled = false;
	bool textHasPlacementProfiles = false;
	Text2DPlacement textOverlayPlacement{};
	Text2DPlacement textScene2DPlacement{};
	std::vector<SceneTextMotionClip> textMotionClips;
	bool gameFlowAutoStart = true;
	// 欠落時は従来の開始順。案内はTextRenderer原本ではなくRuntime文字overrideを使う。
	std::string gameFlowStartSequence = "CountdownFirst";
	std::string gameFlowIntroText = "敵を全滅させよう";
	float gameFlowIntroSeconds = 1.5f;
	uint64_t gameFlowIntroTextEntityId = 0; // 0はカウントダウン用Textと共用。
	std::string gameFlowIntroMotionClipId;
	int gameFlowCountdownStart = 3;
	float gameFlowCountdownStepSeconds = 1.0f;
	std::string gameFlowStartCueText = "START!";
	float gameFlowStartCueSeconds = 0.75f;
	float gameFlowInterPhaseDelaySeconds = 1.0f;
	float gameFlowResultRevealDelaySeconds = 0.75f;
	float gameFlowTimerDisplayStepSeconds = 0.1f;
	std::string gameFlowTimerPrefix = "TIME ";
	std::string gameFlowResultPrefix = "CLEAR TIME ";
	uint64_t gameFlowCountdownTextEntityId = 0;
	std::string gameFlowCountdownMotionClipId;
	uint64_t gameFlowPhaseTextEntityId = 0;
	std::string gameFlowPhaseMotionClipId;
	uint64_t gameFlowTimerTextEntityId = 0;
	uint64_t gameFlowRemainingTextEntityId = 0;
	std::string gameFlowRemainingPrefix = "ENEMIES ";
	uint64_t gameFlowResultRootEntityId = 0;
	uint64_t gameFlowResultTimeTextEntityId = 0;
	std::string gameFlowResultMotionClipId;
	std::vector<SceneGameFlowPhase> gameFlowPhases;
	bool cameraIsMain = false;
	float cameraFovY = 0.45f;
	float cameraNearClip = 0.1f;
	float cameraFarClip = 1000.0f;
	bool cameraInvertYaw = false;
	bool cameraInvertPitch = false;
	std::string lightType = "Point";
	Vector4 lightColor = { 1.0f, 0.85f, 0.65f, 1.0f };
	float lightIntensity = 2.0f;
	float lightRange = 8.0f;
	float lightDecay = 1.0f;
	float lightSpotInnerAngle = 25.0f;
	float lightSpotOuterAngle = 35.0f;
	bool lightCastsShadow = false;
	float lightShadowBias = 0.0025f;
	float lightShadowNormalBias = 0.02f;
	float lightShadowStrength = 0.55f;
	float lightShadowDistance = 45.0f;
	float lightShadowOrthographicSize = 40.0f;
	float lightShadowNearClip = 0.1f;
	float lightShadowFarClip = 120.0f;
	bool lightShadowTexelSnap = true;
	uint64_t monitorCameraEntityId = 0;
	std::string monitorCameraName;
	std::string monitorResolutionPreset = "Square 512";
	uint32_t monitorWidth = 512;
	uint32_t monitorHeight = 512;
	bool monitorHideSelf = true;
	std::string cameraSwitchTriggerKey = "F5";
	bool cameraSwitchWrap = true;
	std::vector<SceneCameraSwitchEntry> cameraSwitchEntries;
	uint64_t thirdPersonTargetEntityId = 0;
	std::string thirdPersonTargetEntityName;
	float thirdPersonDistance = 8.0f;
	float thirdPersonZoomMinDistance = 1.4f;
	float thirdPersonZoomMaxDistance = 30.0f;
	// InspectorとScene検証で共有する通常ズーム範囲の保存条件。
	static bool IsValidThirdPersonZoomRange(float minimum, float maximum);
	float thirdPersonAimDistance = 3.0f;
	Vector3 thirdPersonTargetOffset = { 0.0f, 1.35f, 0.0f };
	Vector3 thirdPersonAimTargetOffset = { 0.0f, 1.55f, 0.0f };
	float thirdPersonMouseSensitivity = 0.005f;
	float thirdPersonMinPitch = -1.45f;
	float thirdPersonMaxPitch = 1.35f;
	float thirdPersonOcclusionMargin = 0.45f;
	uint32_t thirdPersonOcclusionMask = 0xffffffffu;
	float thirdPersonOcclusionPullInSmoothTime = 0.04f;
	float thirdPersonOcclusionRecoverySmoothTime = 0.18f;
	float thirdPersonPositionSmoothTime = 0.12f;
	float thirdPersonRotationSmoothTime = 0.08f;
	std::string thirdPersonYawReference = "World";
	bool thirdPersonAllowMouseInput = true;
	bool thirdPersonOcclusionEnabled = true;
	bool thirdPersonAimModeEnabled = true;
	bool thirdPersonInvertYaw = false;
	bool thirdPersonInvertPitch = false;
	bool animatorPlayOnStart = true;
	bool animatorLoop = true;
	float animatorSpeed = 1.0f;
	int animatorDefaultClip = 0;
	float animatorTransitionDuration = 0.2f;
	std::string animatorBlendCurve = "SmoothStep";
	std::string audioClipPath;
	std::string audioSpatialMode = "TwoD";
	float audioMinimumDistance = 1.4f;
	float audioMaximumDistance = 30.0f;
	float audioStereoAreaWidth = 1.0f;
	std::string audioBus = "SFX";
	float audioVolume = 1.0f;
	float audioPitch = 1.0f;
	bool audioLoop = false;
	bool audioPlayOnStart = false;
	bool audioStopOnDisable = true;
	bool audioDecompressOnLoad = true;
	bool audioStreamFromDisk = false;
	bool audioPersistAcrossScenes = false;
	float audioBgmFadeSeconds = 0.5f;
	std::string audioListenerMode = "Hybrid";
	std::string physicsBodyType = "Static";
	float physicsMass = 1.0f;
	bool physicsUseGravity = true;
	float physicsGravityScale = 1.0f;
	float physicsDrag = 0.0f;
	float physicsRestitution = 0.0f;
	float physicsFriction = 0.0f;
	float physicsMaxFallSpeed = 100.0f;
	Vector3 physicsVelocity = { 0.0f, 0.0f, 0.0f };
	bool physicsFreezePositionX = false;
	bool physicsFreezePositionY = false;
	bool physicsFreezePositionZ = false;
	Vector3 colliderOffset = { 0.0f, 0.0f, 0.0f };
	Vector3 colliderSizeMultiplier = { 1.0f, 1.0f, 1.0f };
	Vector4 colliderDebugColor = { 0.2f, 0.95f, 0.7f, 1.0f };
	std::string colliderShape = "Box";
	float colliderSphereRadius = 0.5f;
	bool colliderDebugVisible = true;
	std::string colliderDebugDrawMode = "Wireframe";
	int colliderDebugSegments = 16;
	bool colliderIsTrigger = false;
	bool colliderActive = true;
	uint32_t colliderLayer = 0xffffffffu;
	uint32_t colliderMask = 0xffffffffu;
	SceneMovementBoundsSettings movementBounds;
	float playerMoveSpeed = 10.8f;
	float playerJumpVelocity = 37.2f;
	float playerTurnResponsiveness = 0.018f;
	float playerDashMultiplier = 1.65f;
	bool playerCameraRelativeMove = true;
	bool playerAllowJump = true;
	std::string agentBehaviorName = "Fish";
	// Free3Dは既存の遊泳群制御、GroundXZはEnemyBehaviorの速度へ離隔だけを加える。
	std::string agentMovementMode = "Free3D";
	std::string agentProfileName = "Default";
	std::string agentGroupName;
	uint64_t agentBoundsEntityId = 0;
	std::string agentBoundsName;
	uint64_t agentAttractorEntityId = 0;
	std::string agentAttractorTag;
	bool agentUseWaterBounds = true;
	float agentMinSpeed = 1.0f;
	float agentMaxSpeed = 3.0f;
	float agentTurnSpeed = 2.5f;
	float agentWanderStrength = 0.8f;
	float agentWanderChangeInterval = 4.0f;
	float agentWanderDirectionRange = 1.1f;
	float agentWanderVerticalRange = 0.18f;
	bool agentRandomizeSeedOnPlay = true;
	int agentRandomSeed = 1;
	float agentFlockDecisionInterval = 0.25f;
	float agentFlockAcceleration = 4.0f;
	float agentFlockTurnRate = 1.5f;
	float agentMemberCenterFollow = 1.5f;
	float agentMemberJitterStrength = 0.35f;
	float agentMemberJitterFrequency = 0.9f;
	float agentMemberJitterUpdateInterval = 0.5f;
	float agentMemberJitterFollowSpeed = 2.0f;
	float agentMemberSpeedVariation = 0.15f;
	float agentMemberLeashDistance = 4.0f;
	float agentMemberLeashStrength = 1.5f;
	float agentMemberCatchupSpeed = 2.0f;
	float agentMemberSeparationUpdateInterval = 0.1f;
	float agentMemberSeparationBlend = 0.5f;
	float agentBoundsWeight = 3.0f;
	bool agentUseTeamHeading = false;
	bool agentTeamHeadingFromAverage = true;
	Vector3 agentTeamHeadingDirection = { 0.0f, 0.0f, 1.0f };
	float agentTeamHeadingWeight = 0.75f;
	float agentTeamHeadingFollowSpeed = 2.5f;
	bool agentUseTeamRotation = false;
	float agentTeamRotationWeight = 0.6f;
	float agentTeamRotationFollowSpeed = 4.0f;
	bool agentAlignForwardToVelocity = true;
	std::string agentForwardAxis = "+Z";
	bool agentRotateAxisX = true;
	bool agentRotateAxisY = true;
	bool agentRotateAxisZ = false;
	float agentRotationFollowSpeed = 12.0f;
	float agentPitchFromVerticalVelocity = 1.0f;
	float agentBankingStrength = 0.0f;
	bool agentSchooling = false;
	float agentSchoolingUpdateInterval = 0.0f;
	float agentSchoolingUpdateJitter = 0.0f;
	int agentNeighborLimit = 0;
	float agentSchoolingBlend = 1.0f;
	float agentSeparationRadius = 1.2f;
	float agentAlignmentRadius = 4.0f;
	float agentCohesionRadius = 5.0f;
	float agentSeparationWeight = 1.8f;
	float agentAlignmentWeight = 0.8f;
	float agentCohesionWeight = 0.9f;
	float agentAttractorWeight = 0.0f;
	bool agentTeamSettingsOverride = false;
	Vector4 agentVisualColor = { 0.25f, 0.75f, 1.0f, 1.0f };
	bool agentEnableLighting = true;
	std::string attractorTag = "Default";
	std::string attractorTargetBehaviorName;
	std::string attractorTargetProfileName;
	float attractorRadius = 6.0f;
	float attractorStrength = 1.0f;
	Vector4 attractorVisualColor = { 1.0f, 0.35f, 0.45f, 1.0f };
	Vector3 waterHalfSize = { 10.0f, 4.0f, 10.0f };
	Vector3 waterOffset = { 0.0f, 0.0f, 0.0f };
	bool waterSurfaceEnabled = true;
	Vector4 waterSurfaceBaseColor = { 0.04f, 0.55f, 0.78f, 1.0f };
	Vector4 waterSurfaceHighlightColor = { 0.42f, 0.95f, 1.20f, 1.0f };
	float waterSurfaceAlpha = 0.36f;
	float waterSurfaceWaveScale = 1.0f;
	float waterSurfaceNormalStrength = 0.75f;
	float waterSurfaceFresnelPower = 3.0f;
	bool waterLightShaftEnabled = true;
	Vector4 waterLightColor = { 0.55f, 0.90f, 1.15f, 1.0f };
	Vector3 waterLightDirection = { -0.25f, -1.0f, 0.18f };
	float waterLightIntensity = 0.55f;
	float waterLightDensity = 0.045f;
	float waterLightCausticsIntensity = 0.35f;
	float waterLightCausticsScale = 0.08f;
	float waterLightCausticsSpeed = 1.0f;
	float waterLightBreakupStrength = 1.0f;
	float waterLightWarpStrength = 1.0f;
	float waterLightNoiseScale = 1.0f;
	int waterLightSampleCount = 16;
	float waterMoveSpeedMultiplier = 0.45f;
	float waterGravityScale = 0.55f;
	float waterDrag = 4.0f;
	float waterMaxFallSpeed = 5.0f;
	float waterSwimUpSpeed = 12.0f;
	std::string entityReferenceName = "Target";
	SceneEntityReference entityReferenceTarget{};
	std::string sceneTransitionTargetSceneId = "gameplay";
	std::string sceneTransitionTriggerType = "Key";
	std::string sceneTransitionTriggerKey = "ENTER";
	std::string cameraPathTargetCameraName;
	std::string cameraPathTriggerType = "Key";
	std::string cameraPathTriggerKey = "C";
	float cameraPathEnterDuration = 0.5f;
	float cameraPathExitDuration = 0.5f;
	std::string cameraPathInterpolation = "Linear";
	std::string cameraPathDefaultEasing = "SmoothStep";
	bool cameraPathReturnToPreviousCamera = true;
	bool cameraPathStartFromCurrentCamera = true;
	bool cameraPathAutoCollectChildPoints = true;
	float cameraPathPointDurationToNext = 1.0f;
	std::string cameraPathPointEasingToNext = "SmoothStep";
	// 表示設定だけを保存する。現在値はStatSystemから借用し、文字原文へ書き戻さない。
	uint64_t statTextStatsEntityId = 0;
	std::string statTextStatsEntityName;
	std::string statTextStatId = "hp";
	std::string statTextDisplayMode = "CurrentAndMax";
	std::string statTextPrefix = "HP ";
	std::string statTextSuffix;
	int statTextDecimalPlaces = 0;
	// Healthは参照と受付設定だけを保存し、HP実値/無敵時計は保存しない。ID 0・名前空欄は自分。
	uint64_t healthStatsEntityId = 0;
	std::string healthStatsEntityName;
	std::string healthStatId = "hp";
	float healthInvulnerabilityDuration = 0.5f;
	std::vector<SceneStatDefinition> stats;
	std::vector<SceneFlagDefinition> flags;
	// DeathReplayは同EntityのDeathResponseだけを使い、実行状態を保存しない。
	bool deathReplayEnabled = false; // trueで記録+3視点再生。欠落した旧Sceneはスローのみ。
	float deathReplaySpeed = 1.0f;
	std::vector<SceneReplayViewSettings> deathReplayViews = DefaultReplayViews();
	float deathReplayCaptureHz = 30.0f;
	uint64_t deathReplayHistoryBudgetMiB = 256;
	float deathReplaySlowScale = 0.2f;
	float deathReplaySlowDuration = 1.5f; // 実秒。世界の進行秒は倍率との積。
	// 同一EntityのHealthを使う。Runtimeの死亡/演出状態は保存しない。
	std::string deathResponseFlagId;
	std::string deathResponseScope = "Target";
	std::vector<SceneAttackDefinition> attackDefinitions;
	std::vector<SceneEventBinding> eventBindings;
	// Menuは設定だけを保存し、開閉・選択・Pause理由はRuntime側が所有する。
	bool menuInitiallyOpen = false;
	// 下層の描画だけを抑止。開閉・選択・Pause状態は保持し、旧Sceneでは無効。
	bool menuHideLowerPanels = false;
	// 欠落時は旧構成を保持。Editorの標準構成操作だけが新方式を選ぶ。
	std::string menuLayoutMode = "Hierarchical";
	std::string menuInputMode = "Events";
	// 非使用軸も保持する。標準操作の実行状態はEventSystem側にのみ置く。
	std::vector<std::string> menuStandardUpKeys{"UP"}, menuStandardDownKeys{"DOWN"};
	std::vector<std::string> menuStandardLeftKeys{"LEFT"}, menuStandardRightKeys{"RIGHT"};
	std::vector<std::string> menuStandardConfirmKeys{"ENTER"}, menuStandardCancelKeys{"ESCAPE"};
	// 順序は上/下/左/右/決定/キャンセル。ポインタはこのComponentの寿命内だけ有効。
	auto MenuStandardKeyGroups() {
		return std::array<std::vector<std::string>*, 6>{&menuStandardUpKeys, &menuStandardDownKeys,
			&menuStandardLeftKeys, &menuStandardRightKeys, &menuStandardConfirmKeys, &menuStandardCancelKeys};
	}
	auto MenuStandardKeyGroups() const {
		return std::array<const std::vector<std::string>*, 6>{&menuStandardUpKeys, &menuStandardDownKeys,
			&menuStandardLeftKeys, &menuStandardRightKeys, &menuStandardConfirmKeys, &menuStandardCancelKeys};
	}
	std::vector<uint64_t> menuItemEntityIds;
	uint64_t menuInitialItemEntityId = 0;
	std::string menuNavigationAxis = "Vertical";
	std::string menuCancelMode = "Close";
	uint64_t menuCancelItemEntityId = 0;
	bool menuInteractable = true;
	uint64_t menuLabelEntityId = 0;
	Vector2 menuTextPadding{};
	Vector2 menuTextOffset{};
	uint64_t menuSelectionMarkerEntityId = 0;
	bool menuSelectedTextColorEnabled = false;
	Vector4 menuSelectedTextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	uint64_t gamePausePanelEntityId = 0;
	bool gamePauseBgm = false;
	std::vector<ScenePrefabAnimationClip> prefabAnimationClips;
	std::string stateMachineInitialState = "Idle";
	bool stateMachineResetOnDisable = true;
	std::vector<SceneStateDefinition> stateMachineStates;
	std::string factionName = "Neutral";
	float hitBoxDamage = 10.0f;
	float hitBoxPoiseDamage = 0.0f;
	float hitBoxKnockback = 0.0f;
	float hitBoxVerticalKnockback = 0.0f;
	std::string hitBoxKnockbackDirectionMode = "RadialFromAttacker";
	Vector3 hitBoxKnockbackLocalDirection = { 0.0f, 0.0f, 1.0f };
	// Runtime専用。Attack Windowが切り替わった接触を別Hitとして扱うための世代番号。
	uint64_t hitBoxAttackWindowSerial = 0;
	// Runtime専用。同一Attack内で同Frameに重なったReactionの優先判定に使う。
	uint64_t hitBoxAttackExecutionId = 0;
	uint32_t hitBoxReactionPriority = 0xffffffffu;
	std::string hitBoxHitPolicy = "OncePerActivation";
	float hitBoxTargetCooldown = 0.15f;
	float hitBoxHitStopDuration = 0.0f;
	std::string hitBoxReactionTag = "Light";
	std::string hitBoxDamageStatId = "hp";
	std::string hitBoxPoiseStatId = "poise";
	uint64_t hitBoxOwnerEntityId = 0;
	std::string hitBoxOwnerEntityName;
	bool hitBoxIgnoreSameFaction = true;
	float hurtBoxDamageMultiplier = 1.0f;
	std::string hurtBoxHealthStatId = "hp";
	uint64_t hurtBoxStatsEntityId = 0;
	std::string hurtBoxStatsEntityName;
	float hitReactionKnockbackMultiplier = 1.0f;
	std::string hitReactionTriggerMode = "MinimumDamage";
	float hitReactionMinimumPoiseDamage = 0.0f;
	std::string hitReactionPoiseStatId = "poise";
	float hitReactionPoiseRecoveryDelay = 1.0f;
	std::string hitReactionStateName = "Hit";
	float hitReactionStateDuration = 0.2f;
	std::string deathPresentationStateName = "Dead";
	float deathPresentationDeactivateDelay = 2.0f;
	std::string deathPresentationEffectPath;
	uint64_t boneAttachmentTargetEntityId = 0;
	std::string boneAttachmentTargetEntityName;
	std::string boneAttachmentJointName;
	// ManualOffsetはEntity Transformをオフセットとして使う。
	// MatchSourceBoneは武器側の指定ボーンを対象ボーンへ一致させる。
	std::string boneAttachmentAlignmentMode = "ManualOffset";
	std::string boneAttachmentSourceJointName;
	bool boneAttachmentInheritScale = true;
	SceneEnemyTargetMode enemyTargetMode = SceneEnemyTargetMode::LegacyReference;
	std::string enemyTargetModeUnknown;
	uint64_t enemyTargetEntityId = 0;
	std::string enemyTargetEntityName = "Player";
	std::string enemySpawnerPrefabPath;
	int enemySpawnerInitialCount = 0;
	int enemySpawnerMaxAlive = 10;
	float enemySpawnerInterval = 1.0f;
	float enemySpawnerRadius = 3.0f;
	bool enemySpawnerAutoStart = true;
	// Instantは旧JSON互換。ScaleUpは描画だけを0〜100%から拡大し、原本Transformを保つ。
	std::string enemySpawnerSpawnAppearance = "Instant";
	float enemySpawnerSpawnStartScale = 0.0f;
	float enemySpawnerSpawnDuration = 0.5f;
	std::string enemySpawnerSpawnCurve = "EaseOutCubic";
	std::string enemyHealthStatId = "hp";
	// 未指定はLegacy。新方式は位置取りのみで、体当たりは後続工程で接続する。
	std::string enemyMovementMode = "Legacy";
	SceneEnemyPositioningSettings enemyPositioning;
	SceneEnemyChargeSettings enemyCharge;
	SceneEnemyInterruptSettings enemyInterruption;
	float enemyDetectionRange = 12.0f;
	float enemyLoseRange = 18.0f;
	float enemyAttackRange = 2.0f;
	float enemyMoveSpeed = 3.0f;
	float enemyTurnSpeed = 8.0f;
	float enemyAttackCooldown = 1.5f;
	float enemyAttackWindup = 0.2f;
	float enemyAttackActiveTime = 0.25f;
	float enemyAttackRecovery = 0.55f;
	int enemyAttackAnimationClip = 0;
	std::string enemyAttackPrefabAnimationClip;
	uint64_t enemyAttackHitBoxEntityId = 0;
	std::string enemyAttackHitBoxEntityName;
	Vector3 projectileDirection = { 0.0f, 0.0f, 1.0f };
	float projectileSpeed = 12.0f;
	float projectileGravity = 0.0f;
	float projectileLifetime = 5.0f;
	bool projectileDestroyOnHit = true;
	uint64_t projectileHomingTargetEntityId = 0;
	std::string projectileHomingTargetEntityName;
	float projectileHomingStrength = 0.0f;
	std::vector<ScenePostProcessProfile> postProcessProfiles;
	uint64_t postProcessStatusTextEntityId = 0;
	std::string postProcessStatusTextEntityName;
	std::string postProcessStatusTextPrefix = "PostEffect: ";
};

struct ScenePrefabLink {
	std::string assetId;
	std::string sourcePath;
	uint64_t instanceRootId = 0;
	uint64_t localId = 0;
};

struct SceneEntity {
	uint64_t id = 0;
	uint64_t parentId = 0;
	std::string name;
	bool folder = false;
	bool folderTeamEnabled = false;
	bool active = true;
	bool locked = false;
	bool runtimeOnly = false;
	// 最外層Prefab Linkの互換表示。実体はprefabLinksへ保持する。
	// 元アセットとの対応がない通常Entityは全て既定値のままにする。
	std::string prefabAssetId;
	std::string prefabSourcePath;
	uint64_t prefabInstanceRootId = 0;
	uint64_t prefabLocalId = 0;
	// 外側から内側の順にPrefab Instance境界を保持する。
	std::vector<ScenePrefabLink> prefabLinks;
	std::string teamName;
	QuaternionTransform transform{};
	std::string modelPath;
	std::string spriteTexturePath;
	Vector2 spriteSize = { 100.0f, 100.0f };
	Vector2 spriteAnchor = { 0.5f, 0.5f };
	Vector4 spriteColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	bool spriteFlipX = false;
	bool spriteFlipY = false;
	std::vector<SceneComponent> components;
};

enum class ScenePrefabOverrideKind {
	EntityProperty,
	ComponentProperty,
	AddedComponent,
	RemovedComponent,
	AddedEntity,
	RemovedEntity,
	StaleEntity
};

struct ScenePrefabPropertyOverride {
	ScenePrefabOverrideKind kind = ScenePrefabOverrideKind::EntityProperty;
	uint64_t entityLocalId = 0;
	uint64_t instanceEntityId = 0;
	uint64_t componentLocalId = 0;
	std::string entityName;
	std::string componentType;
	std::string propertyPath;
	std::string label;
};


class SceneDocumentRead;
class SceneDocument {
public:
	void Clear(const std::string& sceneName = {});

	bool Load(const std::string& filePath);
	// main限定、独立した次Documentへ適用する。backup/Context交換なし。
	// 失敗時は部分変更し得るためcallerが候補Documentを破棄する。
	bool LoadReadCandidate(const SceneDocumentRead& candidate);
	bool Save(const std::string& filePath);

	SceneEntity& CreateEntity(const std::string& name, uint64_t parentId = 0);
	bool RemoveEntity(uint64_t id);
	uint64_t DuplicateEntity(uint64_t id);
	bool SaveEntityBranchAsPrefab(
		uint64_t id,
		const std::string& filePath,
		uint64_t sourceInstanceRootId = 0
	) const;
	bool SaveAsPrefabVariant(
		const std::string& filePath,
		const std::string& basePrefabPath
	) const;
	bool RevertPrefabVariantToBase();
	std::vector<ScenePrefabPropertyOverride> CollectPrefabVariantOverrides()
		const;
	bool ApplyPrefabVariantOverrideToBase(
		const ScenePrefabPropertyOverride& overrideValue
	);
	bool RevertPrefabVariantOverride(
		const ScenePrefabPropertyOverride& overrideValue
	);
	uint64_t InstantiatePrefab(
		const std::string& filePath,
		uint64_t parentId = 0,
		bool runtimeOnly = false
	);
	bool ApplyPrefabInstance(uint64_t rootId);
	bool RevertPrefabInstance(uint64_t rootId);
	bool UnpackPrefabInstance(uint64_t rootId);
	uint64_t FindPrefabInstanceRoot(uint64_t entityId) const;
	std::vector<uint64_t> CollectPrefabInstanceRoots(uint64_t entityId) const;
	std::vector<std::string> CollectPrefabInstanceOverrides(uint64_t rootId) const;
	std::vector<ScenePrefabPropertyOverride> CollectPrefabPropertyOverrides(
		uint64_t rootId
	) const;
	bool ApplyPrefabPropertyOverride(
		uint64_t rootId,
		const ScenePrefabPropertyOverride& overrideValue
	);
	bool RevertPrefabPropertyOverride(
		uint64_t rootId,
		const ScenePrefabPropertyOverride& overrideValue
	);
	bool SetParent(uint64_t id, uint64_t parentId);
	bool MoveEntity(uint64_t id, int direction);
	bool MoveEntityToParent(uint64_t id, uint64_t parentId);
	bool MoveEntityToSibling(uint64_t id, uint64_t siblingId, bool after);
	bool AddComponent(uint64_t id, const std::string& type);
	bool RemoveComponent(uint64_t id, const std::string& type);
	bool IsDescendantOf(uint64_t id, uint64_t potentialAncestorId) const;
	SceneTeamSettings& CreateTeam(const std::string& name);
	bool RenameTeam(const std::string& oldName, const std::string& newName);
	bool RemoveTeam(const std::string& name);
	SceneTeamSettings* FindTeam(const std::string& name);
	const SceneTeamSettings* FindTeam(const std::string& name) const;
	std::string ResolveInheritedFolderTeamName(uint64_t entityId) const;
	const SceneTeamSettings* ResolveEntityTeam(const SceneEntity& entity) const;
	SceneEntity* FindEntity(uint64_t id);
	const SceneEntity* FindEntity(uint64_t id) const;
	SceneEntity* FindEntityByName(const std::string& name);
	const SceneEntity* FindEntityByName(const std::string& name) const;

	const std::string& GetSceneName() const { return sceneName_; }
	void SetSceneName(const std::string& sceneName) {
		sceneName_ = sceneName;
		MarkDirty();
	}
	std::vector<SceneEntity>& GetEntities() { return entities_; }
	const std::vector<SceneEntity>& GetEntities() const { return entities_; }
	std::vector<SceneTeamSettings>& GetTeams() { return teams_; }
	const std::vector<SceneTeamSettings>& GetTeams() const { return teams_; }
	const SceneLightingSettings& GetLightingSettings() const {
		return lightingSettings_;
	}
	void SetLightingSettings(const SceneLightingSettings& settings) {
		lightingSettings_ = settings;
		MarkDirty();
	}
	ScenePostProcessSettings& GetPostProcessSettings() {
		return postProcessSettings_;
	}
	const ScenePostProcessSettings& GetPostProcessSettings() const {
		return postProcessSettings_;
	}
	void SetPostProcessSettings(const ScenePostProcessSettings& settings) {
		postProcessSettings_ = settings;
		MarkDirty();
	}
	const SceneDebugSettings& GetDebugSettings() const {
		return debugSettings_;
	}
	void SetDebugSettings(const SceneDebugSettings& settings) {
		debugSettings_ = settings;
		MarkDirty();
	}
	bool IsDirty() const { return dirty_; }
	const std::string& GetAssetId() const { return assetId_; }
	bool IsPrefabVariant() const { return !variantBaseAssetId_.empty(); }
	const std::string& GetVariantBaseAssetId() const {
		return variantBaseAssetId_;
	}
	const std::string& GetVariantBasePath() const {
		return variantBasePath_;
	}
	const std::string& GetLastLoadError() const { return lastLoadError_; }
	const std::string& GetLastSaveError() const { return lastSaveError_; }
	uint64_t GetRevision() const { return revision_; }
	// 内容差替えの識別子。通常編集では維持し、0は識別不能。JSONへ保存しない。
	uint64_t GetContentGeneration() const { return contentGeneration_.value; }
	// 同じRuntimeのPersistent detach専用。通常コピーと異なり内容世代を引き継ぐ。
	std::unique_ptr<SceneDocument> CloneForRuntimeDetach() const;
	void MarkDirty() {
		dirty_ = true;
		++revision_;
	}
	void MarkClean() { dirty_ = false; }

private:
	// 暗黙のDocument copy/moveでも、他のfieldより先に内容世代を更新する。
	struct ContentGeneration {
		ContentGeneration();
		ContentGeneration(const ContentGeneration&);
		ContentGeneration(ContentGeneration&& source) noexcept;
		ContentGeneration& operator=(const ContentGeneration& source);
		ContentGeneration& operator=(ContentGeneration&& source) noexcept;
		void Renew();
		uint64_t value = 0;
	};
	bool LoadInternal(const std::string& filePath);
	bool ApplyReadCandidate(const SceneDocumentRead& candidate);
	void RebuildNextId();
	void ValidateHierarchy();

	ContentGeneration contentGeneration_;
	std::string sceneName_;
	std::string assetId_;
	std::string variantBaseAssetId_;
	std::string variantBasePath_;
	std::shared_ptr<const SceneDocument> variantBaseSnapshot_;
	std::vector<SceneEntity> entities_;
	std::vector<SceneTeamSettings> teams_;
	SceneLightingSettings lightingSettings_{};
	ScenePostProcessSettings postProcessSettings_{};
	SceneDebugSettings debugSettings_{};
	uint64_t nextId_ = 1;
	bool dirty_ = false;
	uint64_t revision_ = 0;
	std::string lastLoadError_;
	std::string lastSaveError_;
};

// 保存設定だけの構成検査。未設定も理由コードで返し、呼出側が警告/拒否を選ぶ。
const char* ValidateEnemyChargeConfiguration(const SceneDocument& document, const SceneEntity& owner, const SceneComponent& behavior);

// Documentだけの参照検査。実Player IDは値借用で、HP/死亡・攻撃・所有を変更しない。
// issueがNone以外のentityIdは診断用であり、攻撃対象として使用しない。
SceneEnemyTargetResolution ResolveEnemyTarget(const SceneDocument& document, const SceneEntity& owner,
	const SceneComponent& behavior, uint64_t runtimePlayerEntityId);
