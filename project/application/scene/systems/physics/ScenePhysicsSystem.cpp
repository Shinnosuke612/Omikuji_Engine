// 役割: SceneDocumentの物理設定とRuntimeオブジェクトの運動を同期する。
#include "ScenePhysicsSystem.h"

#include "../../../../engine/3d/Object3d.h"
#include "../../../../engine/collision/Collider.h"
#include "../../../../engine/collision/OBBCollider.h"
#include "../../../../engine/collision/SphereCollider.h"
#include "../../../../engine/math/Math.h"
#include "../../../../engine/scene/SceneDocument.h"
#include "../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../engine/scene/SceneTransformResolver.h"
#include "../../../player/Player.h"

#include <algorithm>
#include <cstdint>
#include <cfloat>
#include <cmath>
#include <limits>
#include <string>

namespace {
	bool RayIntersectObb(
		const Vector3& origin,
		const Vector3& direction,
		float maxDistance,
		const OBBCollider& collider,
		float& outDistance
	) {
		const OBBCollider::OBB obb = collider.GetOBB();
		const Vector3 delta = Math::Subtract(obb.center, origin);
		const float halfSizes[3] = {
			obb.halfSize.x, obb.halfSize.y, obb.halfSize.z
		};
		float minDistance = 0.0f;
		float maxHitDistance = maxDistance;
		for (uint32_t index = 0; index < 3; ++index) {
			const Vector3& axis = obb.axis[index];
			const float projectedOrigin = Math::Dot(axis, delta);
			const float projectedDirection = Math::Dot(axis, direction);
			if (std::abs(projectedDirection) > 0.000001f) {
				float first = (projectedOrigin + halfSizes[index]) / projectedDirection;
				float second = (projectedOrigin - halfSizes[index]) / projectedDirection;
				if (first > second) { std::swap(first, second); }
				minDistance = (std::max)(minDistance, first);
				maxHitDistance = (std::min)(maxHitDistance, second);
				if (minDistance > maxHitDistance) { return false; }
			} else if (
				-projectedOrigin - halfSizes[index] > 0.0f ||
				-projectedOrigin + halfSizes[index] < 0.0f
			) {
				return false;
			}
		}
		outDistance = minDistance;
		return outDistance >= 0.0f && outDistance <= maxDistance;
	}

	Vector3 GetObbSurfaceNormal(const OBBCollider& collider, const Vector3& position) {
		const OBBCollider::OBB obb = collider.GetOBB();
		const Vector3 delta = Math::Subtract(position, obb.center);
		const float halfSizes[3] = { obb.halfSize.x, obb.halfSize.y, obb.halfSize.z };
		uint32_t closestAxis = 0;
		float closestDifference = FLT_MAX;
		float projected[3]{};
		for (uint32_t index = 0; index < 3; ++index) {
			projected[index] = Math::Dot(delta, obb.axis[index]);
			const float difference = std::abs(std::abs(projected[index]) - halfSizes[index]);
			if (difference < closestDifference) {
				closestDifference = difference;
				closestAxis = index;
			}
		}
		return Math::Multiply(obb.axis[closestAxis], projected[closestAxis] >= 0.0f ? 1.0f : -1.0f);
	}

	bool RayIntersectSphere(
		const Vector3& origin,
		const Vector3& direction,
		float maxDistance,
		const SphereCollider& collider,
		float& outDistance
	) {
		const Vector3 toCenter = Math::Subtract(collider.GetWorldCenter(), origin);
		const float projectedDistance = Math::Dot(toCenter, direction);
		const float centerDistanceSq = Math::Dot(toCenter, toCenter);
		const float radius = collider.GetRadius();
		const float perpendicularDistanceSq = centerDistanceSq -
			projectedDistance * projectedDistance;
		if (perpendicularDistanceSq > radius * radius) { return false; }
		const float offset = std::sqrt((std::max)(radius * radius - perpendicularDistanceSq, 0.0f));
		outDistance = projectedDistance - offset;
		if (outDistance < 0.0f) { outDistance = projectedDistance + offset; }
		return outDistance >= 0.0f && outDistance <= maxDistance;
	}

	PhysicsBodyType ToPhysicsBodyType(const std::string& bodyType) {
		if (bodyType == "Dynamic") {
			return PhysicsBodyType::Dynamic;
		}
		if (bodyType == "Kinematic") {
			return PhysicsBodyType::Kinematic;
		}
		return PhysicsBodyType::Static;
	}

	bool IsPointInsideWaterVolume(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& waterVolume,
		const Vector3& point
	) {
		const Transform transform =
			SceneTransformResolver::ResolveScene3DTransform(document, entity);
		const Vector3 center = {
			transform.translate.x + waterVolume.waterOffset.x,
			transform.translate.y + waterVolume.waterOffset.y,
			transform.translate.z + waterVolume.waterOffset.z
		};
		const Vector3 halfSize = {
			(std::max)(waterVolume.waterHalfSize.x, 0.001f),
			(std::max)(waterVolume.waterHalfSize.y, 0.001f),
			(std::max)(waterVolume.waterHalfSize.z, 0.001f)
		};

		return
			std::abs(point.x - center.x) <= halfSize.x &&
			std::abs(point.y - center.y) <= halfSize.y &&
			std::abs(point.z - center.z) <= halfSize.z;
	}

	const SceneRuntimeObjectBinding* FindBinding(
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		uint64_t entityId
	) {
		for (const SceneRuntimeObjectBinding& binding : bindings) {
			if (binding.entity && binding.entity->id == entityId) {
				return &binding;
			}
		}
		return nullptr;
	}
}

void ScenePhysicsSystem::ApplyBodyComponent(
	PhysicsBody& body,
	const SceneComponent& component,
	Object3d* object,
	Collider* collider,
	bool resetVelocity
) const {
	const Vector3 runtimeVelocity = body.velocity;
	body.type = ToPhysicsBodyType(component.physicsBodyType);
	body.transform = object ? &object->GetTransform() : nullptr;
	body.syncTransform = [object]() {
		if (object) {
			object->Update();
		}
	};
	body.collider = collider;
	body.mass = (std::max)(component.physicsMass, 0.001f);
	body.useGravity = component.physicsUseGravity;
	body.gravityScale = component.physicsGravityScale;
	body.drag = (std::max)(component.physicsDrag, 0.0f);
	body.restitution = std::clamp(component.physicsRestitution, 0.0f, 1.0f);
	body.friction = std::clamp(component.physicsFriction, 0.0f, 1.0f);
	body.maxFallSpeed = (std::max)(component.physicsMaxFallSpeed, 0.0f);
	body.freezePositionX = component.physicsFreezePositionX;
	body.freezePositionY = component.physicsFreezePositionY;
	body.freezePositionZ = component.physicsFreezePositionZ;
	body.velocity = resetVelocity ? component.physicsVelocity : runtimeVelocity;
}

void ScenePhysicsSystem::SyncSceneSettings(
	const SceneDocument& document,
	Player* player,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	bool editing,
	const SceneRunUpgradeModifiers& upgrades
) {
	ApplyPlayerBehavior(document, player, upgrades);
	ApplyPlayerPhysics(document, player, bindings, editing);
	ApplyWaterVolumes(document, player);
	RebuildStaticColliders(document, bindings);
}

namespace {
	constexpr float kControlledContactTolerance = 0.00001f;

	// Playerの移動Bodyと表示用行列の対応は、照会とStepで同じ短命の借用にする。
	struct PlayerColliderScope {
		Collider* collider = nullptr;
		const Matrix4x4* original = nullptr;
		Vector3 shapeOffset{};
		explicit PlayerColliderScope(Player* player) {
			if (!player || !player->GetObject() || !player->GetCollider()) return;
			player->GetPhysicsBody().SynchronizeTransform();
			collider = player->GetCollider(); original = collider->GetWorldMatrix();
			const auto before = collider->GetWorldCenter();
			collider->SetWorldMatrix(&player->GetObject()->GetWorldMatrix());
			shapeOffset = Math::Subtract(collider->GetWorldCenter(), before);
		}
		~PlayerColliderScope() { if (collider) collider->SetWorldMatrix(original); }
		PlayerColliderScope(const PlayerColliderScope&) = delete;
		PlayerColliderScope& operator=(const PlayerColliderScope&) = delete;
	};

	void ResolveMovementRoot(const SceneDocument& document, const SceneEntity& entity, uint64_t& root, bool& wall) {
		root = entity.id; wall = true;
		const SceneEntity* ancestor = &entity;
		for (size_t depth = 0; ancestor && depth <= document.GetEntities().size(); ++depth, ancestor = document.FindEntity(ancestor->parentId)) {
			const bool actor = SceneEntityQuery::FindEnabledComponent(*ancestor, "EnemyBehavior") ||
				SceneEntityQuery::FindEnabledComponent(*ancestor, "PlayerBehavior");
			if (actor || SceneEntityQuery::FindEnabledComponent(*ancestor, "PhysicsBody")) {
				root = ancestor->id; wall = !actor; break;
			}
		}
	}

	bool IsSupportingContact(const ColliderSweepHit& hit, const Vector3& displacement) {
		return hit.initialOverlap && hit.normal.y > 0.99f && Math::Dot(displacement, hit.normal) >= -0.0001f;
	}

	Vector3 ProbeHalfExtent(const ColliderSweepShape& shape) {
		if (shape.type == Collider::Type::Sphere) return {shape.radius, shape.radius, shape.radius};
		return {
			std::abs(shape.axis[0].x) * shape.halfSize.x + std::abs(shape.axis[1].x) * shape.halfSize.y + std::abs(shape.axis[2].x) * shape.halfSize.z,
			std::abs(shape.axis[0].y) * shape.halfSize.x + std::abs(shape.axis[1].y) * shape.halfSize.y + std::abs(shape.axis[2].y) * shape.halfSize.z,
			std::abs(shape.axis[0].z) * shape.halfSize.x + std::abs(shape.axis[1].z) * shape.halfSize.y + std::abs(shape.axis[2].z) * shape.halfSize.z};
	}

	bool ProbeBoundsOverlap(const ColliderSweepShape& a, const Vector3& delta, const ColliderSweepShape& b) {
		const auto ah = ProbeHalfExtent(a), bh = ProbeHalfExtent(b);
		const double ac[] = {a.center.x, a.center.y, a.center.z}, bc[] = {b.center.x, b.center.y, b.center.z};
		const double av[] = {ah.x, ah.y, ah.z}, bv[] = {bh.x, bh.y, bh.z}, dv[] = {delta.x, delta.y, delta.z};
		for (size_t i = 0; i < 3; ++i) {
			// 丸め誤差で接触候補を落とさない。最終判断は既存sweepの形状判定へ任せる。
			if ((std::max)(ac[i], ac[i] + dv[i]) + av[i] + 0.0001 < bc[i] - bv[i] ||
				(std::min)(ac[i], ac[i] + dv[i]) - av[i] - 0.0001 > bc[i] + bv[i]) return false;
		}
		return true;
	}

	struct ControlledShape {
		uint64_t id = 0, root = 0;
		size_t controlledIndex = (std::numeric_limits<size_t>::max)();
		ColliderSweepShape start;
		Vector3 delta{};
		Collider* collider = nullptr;
		bool valid = false, trigger = false, wall = false;
	};
	struct ControlledPath {
		SceneControlledMotion request;
		SceneControlledMotionResult result;
		PhysicsBody* body = nullptr;
		Vector3 normalDelta{}, normalLocalEnd{};
		Matrix4x4 parentInverse = MakeIdentity4x4();
	};
	bool ValidControlledSamples(const SceneControlledMotion& request) {
		if (request.samples.empty()) return true;
		if (request.samples.size() < 2 || request.samples.size() > 65 || request.displacement.y != 0) return false;
		const auto& first = request.samples.front();
		const auto& last = request.samples.back();
		if (first.frameFraction != request.activeBegin || first.offset.x != 0 || first.offset.y != 0 || first.offset.z != 0 ||
			last.frameFraction != request.activeEnd || last.offset.x != request.displacement.x ||
			last.offset.y != 0 || last.offset.z != request.displacement.z) return false;
		float previous = -1.0f;
		for (const auto& sample : request.samples) {
			if (!std::isfinite(sample.frameFraction) || sample.frameFraction <= previous || sample.frameFraction < request.activeBegin ||
				sample.frameFraction > request.activeEnd || !std::isfinite(sample.offset.x) || sample.offset.y != 0 ||
				!std::isfinite(sample.offset.z)) return false;
			previous = sample.frameFraction;
		}
		return true;
	}
	// Sweepと最終位置が同じ時間付き経路を参照する。Yの通常物理はここに含めない。
	Vector3 ControlledRequestOffset(const SceneControlledMotion& request, float time) {
		if (request.samples.empty()) {
			const float duration = request.activeEnd - request.activeBegin;
			const float rate = duration > 0 ? std::clamp((time - request.activeBegin) / duration, 0.0f, 1.0f) : 0;
			return {request.displacement.x * rate, 0, request.displacement.z * rate};
		}
		if (time <= request.activeBegin) return {};
		if (time >= request.activeEnd) return request.displacement;
		for (size_t i = 1; i < request.samples.size(); ++i) {
			const auto& begin = request.samples[i - 1];
			const auto& end = request.samples[i];
			if (time > end.frameFraction) continue;
			// 有限な大きいoffset同士の差も、中間floatでoverflowさせない。
			const double rate = (double(time) - begin.frameFraction) / (double(end.frameFraction) - begin.frameFraction);
			return {static_cast<float>(begin.offset.x + (double(end.offset.x) - begin.offset.x) * rate), 0,
				static_cast<float>(begin.offset.z + (double(end.offset.z) - begin.offset.z) * rate)};
		}
		return request.displacement;
	}
	Vector3 ControlledOffset(const ControlledShape& shape, const std::vector<ControlledPath>& paths, float time) {
		if (shape.controlledIndex < paths.size()) {
			const auto& path=paths[shape.controlledIndex];
			if (path.result.invalid) return Math::Multiply(shape.delta, time);
			auto offset = ControlledRequestOffset(path.request, (std::min)(time, path.result.fraction));
			offset.y = path.normalDelta.y * time;
			return offset;
		}
		return Math::Multiply(shape.delta, time);
	}
	// 両者の節点/activeBegin/end/停止時刻で分割する。停止後の相手を動き続けると仮定しない。
	ColliderSweepResult ControlledSweep(const ControlledShape& a, const ControlledShape& b,
		const std::vector<ControlledPath>& paths, float begin, float end, ColliderSweepHit& hit) {
		if (!a.valid || !b.valid || !std::isfinite(begin) || !std::isfinite(end) || begin > end) return ColliderSweepResult::Invalid;
		if (begin == end) {
			auto sa=a.start,sb=b.start; sa.center=Math::Add(sa.center,ControlledOffset(a,paths,begin)); sb.center=Math::Add(sb.center,ControlledOffset(b,paths,begin));
			const auto result=SweepColliderTranslation(sa,{},sb,{},hit); if(result==ColliderSweepResult::Hit)hit.fraction=begin; return result;
		}
		std::vector<float> cuts{begin, end};
		// 無関係な敵の段階境界では分割しない。各組の区間数を一定に保つ。
		for(size_t index:{a.controlledIndex,b.controlledIndex}) if(index<paths.size()) {
			const auto& path=paths[index];
			for(float t:{path.request.activeBegin,path.request.activeEnd,path.result.fraction})if(t>begin&&t<end)cuts.push_back(t);
			for (const auto& sample : path.request.samples)
				if (sample.frameFraction > begin && sample.frameFraction < end) cuts.push_back(sample.frameFraction);
		}
		std::sort(cuts.begin(), cuts.end());
		cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
		for (size_t i = 1; i < cuts.size(); ++i) {
			auto sa = a.start, sb = b.start;
			const auto da = ControlledOffset(a, paths, cuts[i - 1]), db = ControlledOffset(b, paths, cuts[i - 1]);
			sa.center = Math::Add(sa.center, da); sb.center = Math::Add(sb.center, db);
			ColliderSweepHit local;
			const auto result = SweepColliderTranslation(sa, Math::Subtract(ControlledOffset(a, paths, cuts[i]), da),
				sb, Math::Subtract(ControlledOffset(b, paths, cuts[i]), db), local);
			if (result == ColliderSweepResult::Invalid) return result;
			if (result == ColliderSweepResult::Hit) {
				local.fraction = cuts[i - 1] + (cuts[i] - cuts[i - 1]) * local.fraction;
				hit = local; return result;
			}
		}
		return ColliderSweepResult::Miss;
	}
	Vector3 ControlledLocalDelta(const Matrix4x4& inverse, const Vector3& world) {
		return {world.x*inverse.m[0][0]+world.y*inverse.m[1][0]+world.z*inverse.m[2][0],
			world.x*inverse.m[0][1]+world.y*inverse.m[1][1]+world.z*inverse.m[2][1],
			world.x*inverse.m[0][2]+world.y*inverse.m[1][2]+world.z*inverse.m[2][2]};
	}
}

SceneMovementProbeSnapshot ScenePhysicsSystem::CaptureMovementProbeSnapshot(const SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings, Player* player, uint64_t frameToken) const {
	SceneMovementProbeSnapshot snapshot;
	snapshot.contentGeneration = document.GetContentGeneration(); snapshot.frameToken = frameToken;
	PlayerColliderScope playerCollider(player);
	const auto* playerEntity = player ? document.FindEntityByName("Player") : nullptr;
	snapshot.shapes.reserve(bindings.size());
	for (const auto& binding : bindings) {
		if (!binding.entity || !binding.collider || !binding.collider->IsActive() || binding.collider->IsTrigger() ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(document, *binding.entity)) continue;
		SceneMovementProbeShape value;
		value.entityId = binding.entityId;
		ResolveMovementRoot(document, *binding.entity, value.rootId, value.wall);
		value.valid = CaptureColliderSweepShape(*binding.collider, value.shape);
		if (playerCollider.collider && playerEntity && value.entityId != playerEntity->id && value.rootId == playerEntity->id)
			value.shape.center = Math::Add(value.shape.center, playerCollider.shapeOffset);
		snapshot.shapes.push_back(value);
	}
	return snapshot;
}

SceneMovementProbeResult ScenePhysicsSystem::ProbeTargetApproach(const SceneMovementProbeSnapshot& snapshot,
	uint64_t ownerId, uint64_t targetId, const Vector3& ownerOffset) {
	SceneMovementProbeResult result;
	const SceneMovementProbeShape* own = nullptr;
	const SceneMovementProbeShape* target = nullptr;
	for (const auto& value : snapshot.shapes) {
		if (value.entityId == ownerId) own = &value;
		if (value.entityId == targetId) target = &value;
	}
	if (!own) return result;
	if (!target) { result.issue = SceneMovementProbeIssue::TargetShape; return result; }
	if (!own->valid || !target->valid || ownerId == targetId || ownerOffset.y != 0 ||
		!std::isfinite(ownerOffset.x) || !std::isfinite(ownerOffset.z)) {
		result.issue = SceneMovementProbeIssue::InvalidShape; return result;
	}
	ColliderSweepShape origin = own->shape;
	origin.center = Math::Add(origin.center, ownerOffset);
	if (!(origin.mask & target->shape.attribute) || !(target->shape.mask & origin.attribute)) {
		result.issue = SceneMovementProbeIssue::CollisionMask; return result;
	}
	Vector3 displacement = Math::Subtract(target->shape.center, origin.center); displacement.y = 0;
	const double length = std::hypot(double(displacement.x), double(displacement.z));
	if (!std::isfinite(length) || length > (std::numeric_limits<float>::max)()) {
		result.issue = SceneMovementProbeIssue::InvalidShape; return result;
	}
	ColliderSweepHit targetHit;
	const auto reach = SweepColliderTranslation(origin, displacement, target->shape, {}, targetHit);
	if (reach == ColliderSweepResult::Invalid) { result.issue = SceneMovementProbeIssue::InvalidShape; return result; }
	if (reach != ColliderSweepResult::Hit || IsSupportingContact(targetHit, displacement)) {
		result.issue = SceneMovementProbeIssue::NoHorizontalContact; return result;
	}
	result.issue = SceneMovementProbeIssue::None;
	result.contactDistance = static_cast<float>(length * targetHit.fraction);
	// 対象より後ろの壁では構えを止めない。実突進中の停止はStepが別途確定する。
	displacement = Math::Multiply(displacement, targetHit.fraction);
	struct Blocker { uint64_t id; float fraction; bool wall; };
	std::vector<Blocker> blockers;
	float firstFraction = 1.0f;
	for (const auto& other : snapshot.shapes) {
		if (other.rootId == own->rootId || other.rootId == target->rootId) continue;
		if (!other.valid) { result.issue = SceneMovementProbeIssue::InvalidShape; return result; }
		if (!(origin.mask & other.shape.attribute) || !(other.shape.mask & origin.attribute) ||
			!ProbeBoundsOverlap(origin, displacement, other.shape)) continue;
		ColliderSweepHit hit;
		const auto sweep = SweepColliderTranslation(origin, displacement, other.shape, {}, hit);
		if (sweep == ColliderSweepResult::Invalid) { result.issue = SceneMovementProbeIssue::InvalidShape; return result; }
		if (sweep != ColliderSweepResult::Hit || IsSupportingContact(hit, displacement)) continue;
		firstFraction = (std::min)(firstFraction, hit.fraction);
		blockers.push_back({other.rootId, hit.fraction, other.wall});
	}
	// Stepと同じく最早接触を先に確定してから同時接触を選ぶ。反復順で許容幅をずらさない。
	for (const auto& blocker : blockers) {
		if (blocker.fraction > firstFraction + kControlledContactTolerance) continue;
		const bool preferred = blocker.wall != result.wall ? blocker.wall : blocker.id < result.blockerId;
		if (!result.blocked || preferred) {
			result.blocked = true; result.wall = blocker.wall; result.blockerId = blocker.id;
		}
	}
	if (result.blocked) result.blockerDistance = result.contactDistance * firstFraction;
	return result;
}

void ScenePhysicsSystem::Step(
	Player* player,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	float deltaTime,
	bool playing,
	const SceneDocument* document,
	const std::vector<SceneControlledMotion>& controlled,
	std::vector<SceneControlledMotionResult>* results
) {
	if (results) results->clear();
	if (!playing) {
		return;
	}

	const auto* playerEntity = document ? document->FindEntityByName("Player") : nullptr;
	const auto* bounds = playerEntity && SceneEntityQuery::IsEntityActiveInHierarchy(*document, *playerEntity)
		? SceneEntityQuery::FindEnabledComponent(*playerEntity, "PlayerMovementBounds") : nullptr;
	const bool hasPlayer = player && player->GetObject();

	// BodyとColliderは非所有なので、現在のbindingsからWorld登録を毎Step再構築する。
	physicsWorld_.Clear();
	for (Collider* collider : staticColliders_) {
		physicsWorld_.AddStaticCollider(collider);
	}
	if (hasPlayer && !bounds) physicsWorld_.AddBody(&player->GetPhysicsBody());
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (
			!binding.entity ||
			!binding.object ||
			!binding.body ||
			SceneEntityQuery::HasComponent(*binding.entity, "PlayerBehavior")
		) {
			continue;
		}
		physicsWorld_.AddBody(binding.body);
	}

	PlayerColliderScope playerCollider(player);
	bool advancePlayer = hasPlayer;
	if (!bounds || (document && boundsGeneration_ != document->GetContentGeneration()) ||
		(playerEntity && boundsOwner_ != playerEntity->id)) {
		boundsSafePose_.reset();
		boundsFrame_ = {};
		boundsIssue_ = nullptr;
	}
	std::optional<Transform> boundsStepStart;
	if (hasPlayer && bounds) {
		const auto before = playerCollider.collider ? playerCollider.collider->GetWorldCenter() : Vector3{};
		advancePlayer = ConstrainPlayer(*player, *document, *playerEntity, *bounds);
		if (playerCollider.collider) {
			playerCollider.shapeOffset = Math::Add(playerCollider.shapeOffset,
				Math::Subtract(playerCollider.collider->GetWorldCenter(), before));
		}
		if (advancePlayer) {
			auto& body = player->GetPhysicsBody();
			boundsStepStart = *body.transform;
			physicsWorld_.AddBody(&body);
		}
	}
	std::vector<ControlledPath> paths;
	std::vector<ControlledShape> shapes;
	if (document && !controlled.empty()) {
		for (const auto& request : controlled) {
			ControlledPath path; path.request = request;
			path.result.ownerId = request.ownerId; path.result.serial = request.serial; path.result.fraction = request.activeEnd;
			const auto* owner = document->FindEntity(request.ownerId);
			for (const auto& binding : bindings) if (binding.entityId == request.ownerId) path.body = binding.body;
			const bool valid = owner && path.body && path.body->type == PhysicsBodyType::Dynamic && path.body->transform &&
				std::isfinite(request.activeBegin) && std::isfinite(request.activeEnd) && request.activeBegin >= 0 &&
				request.activeEnd > request.activeBegin && request.activeEnd <= 1 &&
				std::isfinite(request.displacement.x) && std::isfinite(request.displacement.z) && ValidControlledSamples(request);
			bool parentValid = true;
			if (owner && owner->parentId) {
				const auto* parent = document->FindEntity(owner->parentId);
				if (!parent) parentValid = false;
				else {
					const auto matrix = SceneTransformResolver::ResolveSceneWorldMatrix(*document, *parent);
					const float determinant = Determinant(matrix);
					parentValid = std::isfinite(determinant) && std::abs(determinant) > 1e-8f;
					if (parentValid) path.parentInverse = Inverse(matrix);
				}
			}
			if (!valid || !parentValid || (path.body && (path.body->freezePositionX || path.body->freezePositionZ))) {
				path.result.stopped = path.result.invalid = true;
				path.request.activeBegin=std::isfinite(request.activeBegin)?std::clamp(request.activeBegin,0.0f,1.0f):0.0f;
				path.request.activeEnd=path.request.activeBegin;path.request.displacement={};path.request.samples.clear();path.result.fraction=path.request.activeBegin;
			}
			if (path.body && path.body->transform) {
				path.normalLocalEnd = path.body->transform->translate;
				path.body->velocity.x = path.body->velocity.z = 0;
			}
			paths.push_back(path);
		}
		for (const auto& binding : bindings) {
			if (!binding.entity || !binding.collider || !binding.collider->IsActive() ||
				!SceneEntityQuery::IsEntityActiveInHierarchy(*document, *binding.entity)) continue;
			ControlledShape shape;
			shape.id = binding.entityId; shape.root = binding.entityId; shape.collider = binding.collider;
			shape.trigger = binding.collider->IsTrigger();
			shape.wall = true;
			// 子Colliderも実際の移動rootへまとめる。非突進の相手はrootの通常移動に追従する。
			ResolveMovementRoot(*document, *binding.entity, shape.root, shape.wall);
			for(size_t index=0;index<paths.size();++index) {
				const auto& path=paths[index];
				if(shape.id==path.request.ownerId || document->IsDescendantOf(shape.id,path.request.ownerId)) {
					shape.root=path.request.ownerId;shape.controlledIndex=index;break;
				}
			}
			shape.valid = CaptureColliderSweepShape(*binding.collider, shape.start);
			if(playerCollider.collider && playerEntity && shape.id!=playerEntity->id && shape.root==playerEntity->id)
				shape.start.center=Math::Add(shape.start.center,playerCollider.shapeOffset);
			shapes.push_back(shape);
		}
	}
	physicsWorld_.Step(deltaTime);
	// controlled contactとCombatが読む前に、同じ実ColliderとObjectを補正位置へ同期する。
	if (boundsStepStart && !ConstrainPlayer(*player, *document, *playerEntity, *bounds)) {
		auto& body = player->GetPhysicsBody();
		*body.transform = *boundsStepStart;
		body.SynchronizeTransform();
	}
	if (document && !paths.empty()) {
		for (auto& shape : shapes) {
			ColliderSweepShape end;
			if (!CaptureColliderSweepShape(*shape.collider, end)) shape.valid = false;
			else shape.delta = Math::Subtract(end.center, shape.start.center);
		}
		for(auto& shape:shapes) if(shape.root!=shape.id) {
			for(const auto& root:shapes) if(root.id==shape.root) {shape.delta=root.delta;break;}
		}
		for (auto& path : paths) {
			if (path.body && path.body->transform) path.normalLocalEnd = path.body->transform->translate;
			for (const auto& shape : shapes) if (shape.id == path.request.ownerId) path.normalDelta = shape.delta;
		}
		struct Candidate { size_t path; uint64_t obstacle; ColliderSweepHit hit; bool wall; };
		float previousTime = 0;
		for (size_t iteration = 0; iteration < paths.size(); ++iteration) {
			std::vector<Candidate> candidates;
			float earliest = 2;
			for (size_t i = 0; i < paths.size(); ++i) {
				auto& path = paths[i]; if (path.result.stopped || previousTime>path.request.activeEnd) continue;
				const ControlledShape* own = nullptr;
				for (const auto& shape : shapes) if (shape.id == path.request.ownerId && !shape.trigger) own = &shape;
				if (!own || !own->valid) { path.result.stopped = path.result.invalid = true; path.result.fraction = (std::max)(previousTime,path.request.activeBegin); continue; }
				for (const auto& other : shapes) {
					if (other.trigger || other.root == own->root || !own->collider->CanCollideWith(*other.collider)) continue;
					ColliderSweepHit hit;
					const auto result = ControlledSweep(*own, other, paths, (std::max)(previousTime, path.request.activeBegin), path.request.activeEnd, hit);
					if (result == ColliderSweepResult::Invalid) { path.result.stopped = path.result.invalid = true; path.result.fraction = (std::max)(previousTime,path.request.activeBegin); break; }
					if (result != ColliderSweepResult::Hit) continue;
					// 地面の支持面は横向きの阻止ではない。段差側面はこの除外に入らない。
					if (IsSupportingContact(hit, path.request.displacement)) continue;
					earliest = (std::min)(earliest, hit.fraction);
					candidates.push_back({i, other.root, hit, other.wall});
				}
			}
			if (earliest > 1) break;
			std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
				if (a.wall != b.wall) return a.wall;
				return a.obstacle < b.obstacle;
			});
			for (const auto& candidate : candidates) {
				if (candidate.hit.fraction > earliest + kControlledContactTolerance) continue;
				auto& path = paths[candidate.path]; if (path.result.stopped) continue;
				path.result.stopped = true; path.result.fraction = earliest;
				path.result.obstacleId = candidate.obstacle; path.result.wall = candidate.wall;
				for (auto& other : paths) if (!other.result.stopped && other.request.ownerId == candidate.obstacle &&
					earliest >= other.request.activeBegin && earliest <= other.request.activeEnd) {
					other.result.stopped = true; other.result.fraction = earliest; other.result.obstacleId = path.request.ownerId;
				}
			}
			previousTime = earliest;
		}
		for (auto& path : paths) {
			const auto& request = path.request;
			const ControlledShape* attack = nullptr;
			for (const auto& shape : shapes) if (shape.id == request.hitBoxId) attack = &shape;
			if (!path.result.invalid && attack) for (const auto& hurt : shapes) {
				if (!hurt.trigger || (hurt.id != request.targetId && !document->IsDescendantOf(hurt.id, request.targetId))) continue;
				const auto* entity = document->FindEntity(hurt.id);
				if (!entity || !SceneEntityQuery::FindEnabledComponent(*entity, "HurtBox")) continue;
				ColliderSweepHit hit;
				if (ControlledSweep(*attack, hurt, paths, request.activeBegin, path.result.fraction, hit) != ColliderSweepResult::Hit) continue;
				if (path.result.wall && hit.fraction >= path.result.fraction - kControlledContactTolerance) continue;
				path.result.contacts.push_back({request.ownerId, request.serial, request.hitBoxId, hurt.id, hit.fraction, hit.position, hit.normal});
			}
			Vector3 delta = path.result.invalid ? Vector3{} : ControlledRequestOffset(request, path.result.fraction);
			// 非線形は最後の区間の速度を引き継ぐ。停止後はXZ=0、Yは従来の重力/接地に任せる。
			Vector3 exitVelocity{};
			if (!path.result.stopped && !request.samples.empty()) {
				const auto& last = request.samples.back();
				const auto& previous = request.samples[request.samples.size() - 2];
				const double seconds = double(PhysicsWorld::EffectiveStepDelta(deltaTime)) * (double(last.frameFraction) - previous.frameFraction);
				exitVelocity.x = static_cast<float>((double(last.offset.x) - previous.offset.x) / seconds);
				exitVelocity.z = static_cast<float>((double(last.offset.z) - previous.offset.z) / seconds);
				if (!(seconds > 0) || !std::isfinite(exitVelocity.x) || !std::isfinite(exitVelocity.z)) {
					path.result.stopped = path.result.invalid = true; delta = {}; exitVelocity = {};
				}
			}
			float distance = static_cast<float>(std::hypot(double(delta.x),double(delta.z)));
			if(!std::isfinite(distance)){path.result.stopped=path.result.invalid=true;distance=0;delta={};}
			if (path.result.stopped && distance > 0) {
				delta = Math::Multiply(delta, (std::max)(0.0f, distance - 0.0001f) / distance);
				distance = static_cast<float>(std::hypot(double(delta.x),double(delta.z)));
			}
			path.result.distance = path.result.invalid ? 0.0f : distance;
			if (path.body && path.body->transform) {
				if (!path.result.invalid) {
					const auto destination=Math::Add(path.normalLocalEnd,ControlledLocalDelta(path.parentInverse,delta));
					if(std::isfinite(destination.x)&&std::isfinite(destination.y)&&std::isfinite(destination.z))path.body->transform->translate=destination;
					else {path.result.stopped=path.result.invalid=true;path.result.distance=0;}
				}
				if (request.samples.empty()) {
					const float seconds = PhysicsWorld::EffectiveStepDelta(deltaTime) * (request.activeEnd - request.activeBegin);
					path.body->velocity.x = !path.result.stopped && seconds > 0 ? request.displacement.x / seconds : 0;
					path.body->velocity.z = !path.result.stopped && seconds > 0 ? request.displacement.z / seconds : 0;
				} else {
					path.body->velocity.x = path.result.stopped ? 0 : exitVelocity.x;
					path.body->velocity.z = path.result.stopped ? 0 : exitVelocity.z;
				}
				path.body->SynchronizeTransform();
			}
			if(path.result.invalid)path.result.contacts.clear();
			if (results) results->push_back(std::move(path.result));
		}
	}


	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (
			!binding.entity ||
			!binding.object ||
			!binding.body ||
			SceneEntityQuery::HasComponent(*binding.entity, "PlayerBehavior")
		) {
			continue;
		}
		binding.object->Update();
		const Transform& runtimeTransform = binding.object->GetTransform();
		binding.entity->transform.scale = runtimeTransform.scale;
		binding.entity->transform.rotate = runtimeTransform.useQuaternionRotation
			? runtimeTransform.quaternionRotate
			: MakeQuaternionFromEuler(runtimeTransform.rotate);
		binding.entity->transform.translate = runtimeTransform.translate;
	}
}

void ScenePhysicsSystem::ResetBodies(
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	const std::vector<uint64_t>& entityIds
) const {
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (!binding.entity || !binding.body ||
			std::find(entityIds.begin(), entityIds.end(), binding.entity->id) ==
				entityIds.end()) {
			continue;
		}
		const SceneComponent* component =
			SceneEntityQuery::FindEnabledComponent(*binding.entity, "PhysicsBody");
		if (component) {
			binding.body->velocity = component->physicsVelocity;
		}
	}
}

bool ScenePhysicsSystem::RaycastStatic(
	const Vector3& origin,
	const Vector3& direction,
	float maxDistance,
	SceneStaticRaycastHit& outHit
) const {
	const float safeDistance = (std::max)(maxDistance, 0.0f);
	if (safeDistance <= 0.0f || Math::Length(direction) <= 0.000001f) {
		return false;
	}
	const Vector3 normalizedDirection = Math::Normalize(direction);
	float closestDistance = safeDistance;
	const Collider* closestCollider = nullptr;
	bool found = false;
	for (const Collider* collider : staticColliders_) {
		if (!collider || !collider->IsActive() || collider->IsTrigger()) {
			continue;
		}
		float distance = 0.0f;
		const bool hit = collider->GetType() == Collider::Type::OBB
			? RayIntersectObb(
				origin, normalizedDirection, closestDistance,
				static_cast<const OBBCollider&>(*collider), distance
			)
			: RayIntersectSphere(
				origin, normalizedDirection, closestDistance,
				static_cast<const SphereCollider&>(*collider), distance
			);
		if (!hit) { continue; }
		closestDistance = distance;
		closestCollider = collider;
		found = true;
	}
	if (found) {
		outHit.distance = closestDistance;
		outHit.position = Math::Add(
			origin, Math::Multiply(normalizedDirection, closestDistance)
		);
		outHit.normal = closestCollider->GetType() == Collider::Type::OBB
			? GetObbSurfaceNormal(static_cast<const OBBCollider&>(*closestCollider), outHit.position)
			: Math::Normalize(Math::Subtract(
				outHit.position,
				static_cast<const SphereCollider&>(*closestCollider).GetWorldCenter()
			));
	}
	return found;
}

bool ScenePhysicsSystem::ConstrainPlayer(Player& player, const SceneDocument& document,
	const SceneEntity& owner, const SceneComponent& component) {
	boundsGeneration_ = document.GetContentGeneration();
	boundsOwner_ = owner.id;
	boundsFrame_ = {};
	boundsIssue_ = SceneMovementBounds::TargetIssue(document, owner);
	auto& body = player.GetPhysicsBody();
	const auto& settings = component.movementBounds;
	if (!boundsIssue_) boundsIssue_ = SceneMovementBounds::SettingsIssue(settings);
	auto stop = [&](const char* reason) {
		boundsIssue_ = reason;
		body.velocity = {};
		return false;
	};
	if (boundsIssue_) return stop(boundsIssue_);
	if (!body.transform || !body.collider || !body.collider->IsActive() || body.collider->IsTrigger())
		return stop("MovementBounds:Collider");

	SceneMovementBoundsPlacement placement;
	auto solve = [&] {
		ColliderSweepShape shape;
		return CaptureColliderSweepShape(*body.collider, shape) &&
			SceneMovementBounds::Solve(settings, shape, placement);
	};
	const Transform candidate = *body.transform;
	constexpr float tolerance = 1.0e-5f;
	auto needsCorrection = [&] {
		return std::abs(placement.correction.x) > tolerance || std::abs(placement.correction.y) > tolerance;
	};
	bool accepted = solve();
	bool restored = false;
	if (accepted && needsCorrection()) {
		const Vector3 delta{placement.correction.x, 0, placement.correction.y};
		accepted = (!body.freezePositionX || std::abs(delta.x) <= tolerance) &&
			(!body.freezePositionZ || std::abs(delta.z) <= tolerance) &&
			!physicsWorld_.IsPlanarCorrectionBlocked(body, delta);
		if (accepted) {
			body.transform->translate.x += delta.x;
			body.transform->translate.z += delta.z;
			body.SynchronizeTransform();
			accepted = solve() && !needsCorrection() && !physicsWorld_.IsPlacementBlocked(body);
		}
	}
	if (!accepted && boundsSafePose_) {
		// 壁と境界が競合した場合、押し戻しを反復せず前回の安全姿勢を再検査する。
		*body.transform = *boundsSafePose_;
		body.SynchronizeTransform();
		accepted = solve() && !needsCorrection() && !physicsWorld_.IsPlacementBlocked(body);
		restored = accepted;
	}
	if (!accepted) {
		*body.transform = candidate;
		body.SynchronizeTransform();
		return stop("MovementBounds:Placement");
	}
	if (!physicsWorld_.IsPlacementBlocked(body)) boundsSafePose_ = *body.transform;
	if (restored && body.transform->translate.y != candidate.translate.y) body.velocity.y = 0;
	const auto velocity = restored ? Vector2{} : SceneMovementBounds::RestrictVelocity(placement, {body.velocity.x, body.velocity.z});
	body.velocity.x = velocity.x;
	body.velocity.z = velocity.y;

	if (settings.showBoundary) {
		const double approach = std::clamp(1.0 - placement.clearance / settings.revealDistance, 0.0, 1.0);
		boundsFrame_ = {settings, static_cast<float>(approach * approach * (3.0 - 2.0 * approach))};
	}
	return true;
}

SceneMovementBoundsFrame ScenePhysicsSystem::MovementBoundsFrame(const SceneDocument& document, bool editing) const {
	const auto* owner = document.FindEntityByName("Player");
	if (!owner || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *owner)) return {};
	if (editing) {
		const auto* component = SceneEntityQuery::FindComponent(*owner, "PlayerMovementBounds");
		if (component && !SceneMovementBounds::SettingsIssue(component->movementBounds))
			return {component->movementBounds, 1.0f};
		return {};
	}
	if (boundsGeneration_ != document.GetContentGeneration() || boundsOwner_ != owner->id ||
		!SceneEntityQuery::FindEnabledComponent(*owner, "PlayerMovementBounds")) return {};
	return boundsFrame_;
}

const char* ScenePhysicsSystem::MovementBoundsIssue(const SceneDocument& document) const {
	if (!boundsIssue_ || boundsGeneration_ != document.GetContentGeneration()) return nullptr;
	const auto* owner = document.FindEntity(boundsOwner_);
	return owner && SceneEntityQuery::IsEntityActiveInHierarchy(document, *owner) &&
		SceneEntityQuery::FindEnabledComponent(*owner, "PlayerMovementBounds") ? boundsIssue_ : nullptr;
}

void ScenePhysicsSystem::Clear() {
	boundsGeneration_ = boundsOwner_ = 0;
	boundsSafePose_.reset();
	boundsFrame_ = {};
	boundsIssue_ = nullptr;
	physicsWorld_.Clear();
	staticColliders_.clear();
}

void ScenePhysicsSystem::ApplyPlayerBehavior(
	const SceneDocument& document,
	Player* player,
	const SceneRunUpgradeModifiers& upgrades
) const {
	if (!player) {
		return;
	}
	const SceneEntity* playerEntity = document.FindEntityByName("Player");
	const SceneComponent* behavior = playerEntity
		? SceneEntityQuery::FindEnabledComponent(*playerEntity, "PlayerBehavior")
		: nullptr;
	if (!behavior) {
		return;
	}

	// 毎回基礎値から計算し、Dash/水中補正は従来の後段に委ねる。
	const float moveSpeed = behavior->playerMoveSpeed * upgrades.ForOwner(playerEntity->id).moveSpeed;
	player->SetBehaviorSettings(
		std::isfinite(moveSpeed) ? moveSpeed : behavior->playerMoveSpeed,
		behavior->playerJumpVelocity,
		behavior->playerTurnResponsiveness,
		behavior->playerDashMultiplier,
		behavior->playerCameraRelativeMove,
		behavior->playerAllowJump
	);
}

void ScenePhysicsSystem::ApplyPlayerPhysics(
	const SceneDocument& document,
	Player* player,
	const std::vector<SceneRuntimeObjectBinding>& bindings,
	bool editing
) const {
	if (!player || !player->GetObject()) {
		return;
	}
	const SceneEntity* playerEntity = document.FindEntityByName("Player");
	if (
		!playerEntity ||
		!SceneEntityQuery::HasComponent(*playerEntity, "PlayerBehavior")
	) {
		return;
	}

	const SceneRuntimeObjectBinding* binding =
		FindBinding(bindings, playerEntity->id);
	player->SetCollider(binding ? binding->collider : nullptr);
	const SceneComponent* component =
		SceneEntityQuery::FindEnabledComponent(*playerEntity, "PhysicsBody");
	if (!component) {
		return;
	}

	PhysicsBody& body = player->GetPhysicsBody();
	ApplyBodyComponent(
		body,
		*component,
		player->GetObject(),
		player->GetCollider(),
		editing
	);
	if (body.type == PhysicsBodyType::Static) {
		body.type = PhysicsBodyType::Dynamic;
	}
}

void ScenePhysicsSystem::ApplyWaterVolumes(
	const SceneDocument& document,
	Player* player
) const {
	if (!player || !player->GetObject()) {
		return;
	}
	player->SetWaterState(false, 1.0f, 0.0f);

	const Vector3 playerPosition =
		player->GetObject()->GetTransform().translate;
	for (const SceneEntity& entity : document.GetEntities()) {
		if (!SceneEntityQuery::IsEntityActiveInHierarchy(document, entity)) {
			continue;
		}
		const SceneComponent* waterVolume =
			SceneEntityQuery::FindEnabledComponent(entity, "WaterVolume");
		if (
			!waterVolume ||
			!IsPointInsideWaterVolume(
				document,
				entity,
				*waterVolume,
				playerPosition
			)
		) {
			continue;
		}

		player->SetWaterState(
			true,
			waterVolume->waterMoveSpeedMultiplier,
			waterVolume->waterSwimUpSpeed
		);
		PhysicsBody& body = player->GetPhysicsBody();
		body.gravityScale = waterVolume->waterGravityScale;
		body.drag = (std::max)(body.drag, waterVolume->waterDrag);
		body.maxFallSpeed =
			(std::max)(waterVolume->waterMaxFallSpeed, 0.0f);
		return;
	}
}

void ScenePhysicsSystem::RebuildStaticColliders(
	const SceneDocument& document,
	const std::vector<SceneRuntimeObjectBinding>& bindings
) {
	staticColliders_.clear();
	for (const SceneRuntimeObjectBinding& binding : bindings) {
		if (
			!binding.entity ||
			!binding.collider ||
			binding.body ||
			SceneEntityQuery::HasComponent(
				*binding.entity,
				"PlayerBehavior"
			) ||
			!SceneEntityQuery::IsEntityActiveInHierarchy(
				document,
				*binding.entity
			)
		) {
			continue;
		}
		staticColliders_.push_back(binding.collider);
	}
}
