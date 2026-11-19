// 役割: Box/Circleと移動Colliderの投影を比較し、位置と速度の制限を求める。
#include "SceneMovementBounds.h"
#include "../collision/Collider.h"
#include "../collision/OBBCollider.h"
#include "../collision/SphereCollider.h"
#include "SceneTransformResolver.h"
#include "SceneDocument.h"
#include "SceneEntityQuery.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
	struct Point { double x, z; };
	double Dot(Point a, Point b) { return a.x * b.x + a.z * b.z; }
	bool Positive(float n) { return std::isfinite(n) && n > 0.0f; }
	bool Channel(float n) { return n >= 0.0f && n <= 1.0f; }
}

const char* SceneMovementBounds::SettingsIssue(const SceneMovementBoundsSettings& s) {
	if (s.shape != "Box" && s.shape != "Circle") return "MovementBounds:Shape";
	if (!std::isfinite(s.center.x) || !std::isfinite(s.center.y) ||
		!Positive(s.halfExtents.x) || !Positive(s.halfExtents.y) || !Positive(s.radius))
		return "MovementBounds:Dimensions";
	if (!Positive(s.revealDistance) || !std::isfinite(s.boundaryY) || !Positive(s.lineWidth) ||
		!Channel(s.color.x) || !Channel(s.color.y) || !Channel(s.color.z) || !Channel(s.color.w))
		return "MovementBounds:Appearance";
	return nullptr;
}

const char* SceneMovementBounds::TargetIssue(const SceneDocument& document, const SceneEntity& owner) {
	using SceneEntityQuery::FindEnabledComponent;
	if (owner.name != "Player" || owner.parentId || owner.folder ||
		document.FindEntityByName("Player") != &owner || !FindEnabledComponent(owner, "PlayerBehavior") ||
		std::count_if(document.GetEntities().begin(), document.GetEntities().end(),
			[](const SceneEntity& entity) { return entity.name == "Player"; }) != 1)
		return "MovementBounds:Player";
	const auto* body = FindEnabledComponent(owner, "PhysicsBody");
	if (!body || body->physicsBodyType != "Dynamic")
		return "MovementBounds:Body";
	const auto* collider = FindEnabledComponent(owner, "OBBCollider");
	if (!collider || !collider->colliderActive || collider->colliderIsTrigger)
		return "MovementBounds:Collider";
	return nullptr;
}

const char* SceneMovementBounds::AuthoringIssue(const SceneDocument& document, const SceneEntity& owner,
	const SceneMovementBoundsSettings& settings) {
	if (const char* issue = SettingsIssue(settings)) return issue;
	if (const char* issue = TargetIssue(document, owner)) return issue;
	const auto& collider = *SceneEntityQuery::FindEnabledComponent(owner, "OBBCollider");
	const auto world = SceneTransformResolver::ResolveSceneWorldMatrix(document, owner);
	ColliderSweepShape shape;
	// RuntimeのOBBCollider設定と同じworld形状を一時的な値から構築する。
	if (collider.colliderShape == "Sphere") {
		SphereCollider sphere;
		sphere.SetWorldMatrix(&world);
		sphere.SetOffset(collider.colliderOffset);
		sphere.SetRadius((std::max)(collider.colliderSphereRadius, 0.001f));
		if (!CaptureColliderSweepShape(sphere, shape)) return "MovementBounds:Collider";
	} else {
		OBBCollider box;
		box.SetWorldMatrix(&world);
		box.SetOffset(collider.colliderOffset);
		const auto& h = collider.colliderSizeMultiplier;
		box.SetHalfSize({(std::max)(std::abs(h.x), 0.001f), (std::max)(std::abs(h.y), 0.001f),
			(std::max)(std::abs(h.z), 0.001f)});
		if (!CaptureColliderSweepShape(box, shape)) return "MovementBounds:Collider";
	}
	SceneMovementBoundsPlacement placement;
	return Solve(settings, shape, placement) ? nullptr : "MovementBounds:Fit";
}

bool SceneMovementBounds::Solve(const SceneMovementBoundsSettings& s, const ColliderSweepShape& shape,
	SceneMovementBoundsPlacement& out) {
	out = {};
	if (SettingsIssue(s) || !std::isfinite(shape.center.x) || !std::isfinite(shape.center.z)) return false;
	const Point start{double(shape.center.x) - s.center.x, double(shape.center.z) - s.center.y};
	Point position = start;
	const bool sphere = shape.type == Collider::Type::Sphere;
	if (sphere && !Positive(shape.radius)) return false;
	const double margin = sphere ? shape.radius : 0.0;
	std::array<Point, 8> offsets{};
	const std::size_t count = sphere ? 1 : offsets.size();
	if (!sphere) {
		const float half[] = {shape.halfSize.x, shape.halfSize.y, shape.halfSize.z};
		for (std::size_t i = 0; i < count; ++i) {
			for (std::size_t axis = 0; axis < 3; ++axis) {
				const double extent = (i & (1u << axis)) ? half[axis] : -double(half[axis]);
				offsets[i].x += extent * shape.axis[axis].x;
				offsets[i].z += extent * shape.axis[axis].z;
			}
			if (!std::isfinite(offsets[i].x) || !std::isfinite(offsets[i].z)) return false;
		}
	}
	// floatの位置へ書き戻す際に境界の丸め誤差で押し戻しが反復しないようにする。
	constexpr double tolerance = 1.0e-5;
	auto normal = [&](double x, double z) {
		out.outwardNormals[out.normalCount++] = {static_cast<float>(x), static_cast<float>(z)};
	};
	if (s.shape == "Box") {
		Point extents{margin, margin};
		for (std::size_t i = 0; i < count; ++i) {
			extents.x = (std::max)(extents.x, std::abs(offsets[i].x));
			extents.z = (std::max)(extents.z, std::abs(offsets[i].z));
		}
		const Point limit{s.halfExtents.x - extents.x, s.halfExtents.y - extents.z};
		if (limit.x < 0 || limit.z < 0) return false;
		position = {std::clamp(start.x, -limit.x, limit.x), std::clamp(start.z, -limit.z, limit.z)};
		if (limit.x - position.x <= tolerance) normal(1, 0);
		if (limit.x + position.x <= tolerance) normal(-1, 0);
		if (limit.z - position.z <= tolerance) normal(0, 1);
		if (limit.z + position.z <= tolerance) normal(0, -1);
		out.clearance = (std::min)(limit.x - std::abs(position.x), limit.z - std::abs(position.z));
	} else {
		const double radius = s.radius - margin;
		if (radius < 0) return false;
		if (radius == 0 && sphere) {
			position = {};
			normal(1, 0);
			normal(-1, 0);
			normal(0, 1);
			normal(0, -1);
		} else {
			const double a = Dot(start, start);
			double scale = 1.0;
			for (std::size_t i = 0; i < count; ++i) {
				const Point offset = offsets[i];
				const double c = Dot(offset, offset) - radius * radius;
				if (c > 0) return false;
				const Point end{start.x + offset.x, start.z + offset.z};
				if (Dot(end, end) <= radius * radius) continue;
				// 各頂点の円を出る根を比較し、最初に接触する頂点まで戻す。
				const double b = Dot(start, offset);
				const double root = std::sqrt(b * b - a * c);
				const double exit = b > 0 ? -c / (root + b) : (root - b) / a;
				scale = (std::min)(scale, exit);
			}
			position = {start.x * scale, start.z * scale};
			out.clearance = radius;
			for (std::size_t i = 0; i < count; ++i) {
				const Point point{position.x + offsets[i].x, position.z + offsets[i].z};
				const double length = std::hypot(point.x, point.z);
				out.clearance = (std::min)(out.clearance, radius - length);
				if (length > 0 && radius - length <= tolerance) normal(point.x / length, point.z / length);
			}
		}
	}
	out.correction = {static_cast<float>(position.x - start.x), static_cast<float>(position.z - start.z)};
	return std::isfinite(out.correction.x) && std::isfinite(out.correction.y);
}

Vector2 SceneMovementBounds::RestrictVelocity(const SceneMovementBoundsPlacement& placement, Vector2 velocity) {
	if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y)) return {};
	const Point desired{velocity.x, velocity.y};
	auto allowed = [&](Point candidate) {
		for (std::size_t i = 0; i < placement.normalCount; ++i) {
			const auto n = placement.outwardNormals[i];
			if (Dot(candidate, {n.x, n.y}) > 1.0e-7) return false;
		}
		return true;
	};
	if (allowed(desired)) return velocity;
	Point result{};
	double bestError = Dot(desired, desired);
	// 2Dの許容錐の最近点は各接線上、または原点にある。
	for (std::size_t i = 0; i < placement.normalCount; ++i) {
		const auto direction = placement.outwardNormals[i];
		const Point n{direction.x, direction.y};
		const double outward = Dot(desired, n) / Dot(n, n);
		const Point candidate{desired.x - outward * n.x, desired.z - outward * n.z};
		const Point difference{candidate.x - desired.x, candidate.z - desired.z};
		const double error = Dot(difference, difference);
		if (error < bestError && allowed(candidate)) {
			result = candidate;
			bestError = error;
		}
	}
	const double maxFloat = (std::numeric_limits<float>::max)();
	if (std::abs(result.x) > maxFloat || std::abs(result.z) > maxFloat) return {};
	return {static_cast<float>(result.x), static_cast<float>(result.z)};
}
