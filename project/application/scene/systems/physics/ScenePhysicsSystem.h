// 役割: Sceneの物理Componentを実行状態へ反映し、物理ワールドを更新する。
#pragma once

#include <cstdint>
#include <vector>

#include "../../SceneRuntimeObjectBinding.h"
#include "../../../../engine/physics/PhysicsWorld.h"
#include "../../../../engine/collision/Collider.h"
#include "../../../../engine/scene/SceneMovementBounds.h"
#include <optional>

class Collider;
class Object3d;
class Player;
class SceneDocument;
struct SceneComponent;

struct SceneStaticRaycastHit {
	Vector3 position{};
	Vector3 normal{ 0.0f, 1.0f, 0.0f };
	float distance = 0.0f;
};

// Frame内時刻（0..1）と要求開始からの累積world XZ変位。距離割合ではない。
struct SceneControlledMotionSample {
	float frameFraction = 0.0f;
	Vector3 offset{};
};

// 一Frameのworld XZ移動区間。要求/結果は値だけで、BodyはStep中にbindingsから解決する。
struct SceneControlledMotion {
	uint64_t ownerId = 0, serial = 0, targetId = 0, hitBoxId = 0;
	Vector3 displacement{};
	float activeBegin = 0.0f, activeEnd = 1.0f;
	// 空列は従来の一定速度。非空は2..65点、時刻昇順、両端はbegin/0とend/displacement。
	// 不正な列はStepがInvalidで停止する。経路に曲線名/敵の状態を持ち込まない。
	std::vector<SceneControlledMotionSample> samples;
};
struct SceneControlledContact {
	uint64_t ownerId = 0, serial = 0, hitBoxId = 0, hurtBoxId = 0;
	float fraction = 0.0f;
	Vector3 position{}, normal{};
};
struct SceneControlledMotionResult {
	uint64_t ownerId = 0, serial = 0, obstacleId = 0;
	float fraction = 1.0f, distance = 0.0f;
	bool stopped = false, invalid = false, wall = false;
	std::vector<SceneControlledContact> contacts;
};

// 同期点で採取した移動実体の値コピー。借用ポインタを含まず、そのFrame/内容世代だけで使う。
struct SceneMovementProbeShape {
	uint64_t entityId = 0, rootId = 0;
	ColliderSweepShape shape;
	bool valid = false, wall = true;
};
struct SceneMovementProbeSnapshot {
	uint64_t contentGeneration = 0, frameToken = 0;
	std::vector<SceneMovementProbeShape> shapes;
};
enum class SceneMovementProbeIssue { None, OwnerShape, TargetShape, InvalidShape, CollisionMask, NoHorizontalContact };
struct SceneMovementProbeResult {
	SceneMovementProbeIssue issue = SceneMovementProbeIssue::OwnerShape;
	float contactDistance = 0.0f, blockerDistance = 0.0f;
	uint64_t blockerId = 0;
	bool blocked = false, wall = false;
};

// PhysicsWorldを所有するが、Collider・PhysicsBody・Playerは外部から借用する。
// Object同期後にSyncSceneSettingsを呼び、Object破棄前にClearする必要がある。
class ScenePhysicsSystem {
public:
	void ApplyBodyComponent(
		PhysicsBody& body,
		const SceneComponent& component,
		Object3d* object,
		Collider* collider,
		bool resetVelocity
	) const;
	void SyncSceneSettings(
		const SceneDocument& document,
		Player* player,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		bool editing
	);
	// controlledは同Frameのworld XZ区間。既存の重力/通常移動後に連続接触を解き、結果だけを返す。
	// 不正形状/非可逆parentは当該突進を停止。resultsは初めにclearし、DamageやEntity追加は行わない。
	void Step(
		Player* player,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		float deltaTime,
		bool playing,
		const SceneDocument* document = nullptr,
		const std::vector<SceneControlledMotion>& controlled = {},
		std::vector<SceneControlledMotionResult>* results = nullptr
	);
	// Object同期後に採取する。Playerの行列は実Bodyへ一時同期し、借用行列を復元する。
	// document/設定/速度は変更しない。戻り値はSceneやColliderを所有・参照しない。
	SceneMovementProbeSnapshot CaptureMovementProbeSnapshot(const SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings, Player* player, uint64_t frameToken) const;
	// XZ上で対象の移動実体へ触れるまでの移動量と、その手前の遮蔽物を返す。
	// offsetは候補位置のworld並進。命中/Damageや将来の旋回・相手の移動は判定しない。
	static SceneMovementProbeResult ProbeTargetApproach(const SceneMovementProbeSnapshot& snapshot,
		uint64_t ownerId, uint64_t targetId, const Vector3& ownerOffset = {});
	void ResetBodies(
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		const std::vector<uint64_t>& entityIds
	) const;
	// Static Colliderだけを対象にし、短命の地面Effectを動的Bodyへ重ねない。
	bool RaycastStatic(
		const Vector3& origin,
		const Vector3& direction,
		float maxDistance,
		SceneStaticRaycastHit& outHit
	) const;
	// 内容世代とPlayer IDが一致する確定済みの範囲だけを描画へ渡す。
	SceneMovementBoundsFrame MovementBoundsFrame(const SceneDocument& document, bool editing) const;
	const char* MovementBoundsIssue(const SceneDocument& document) const;
	void Clear();

private:
	bool ConstrainPlayer(Player& player, const SceneDocument& document, const SceneEntity& owner,
		const SceneComponent& component);
	// 姿勢と診断はRuntime専用。DocumentやColliderの借用ポインタを残さない。
	uint64_t boundsGeneration_ = 0, boundsOwner_ = 0;
	std::optional<Transform> boundsSafePose_;
	SceneMovementBoundsFrame boundsFrame_;
	const char* boundsIssue_ = nullptr;
	void ApplyPlayerBehavior(
		const SceneDocument& document,
		Player* player
	) const;
	void ApplyPlayerPhysics(
		const SceneDocument& document,
		Player* player,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		bool editing
	) const;
	void ApplyWaterVolumes(
		const SceneDocument& document,
		Player* player
	) const;
	void RebuildStaticColliders(
		const SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings
	);

	PhysicsWorld physicsWorld_;
	std::vector<Collider*> staticColliders_;
};
