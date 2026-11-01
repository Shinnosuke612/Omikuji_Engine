// 役割: Colliderの共通座標計算と、固定形状同士の並進Sweepを副作用なしで評価する。
#include "Collider.h"

#include "../math/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include "OBBCollider.h"
#include "SphereCollider.h"

bool Collider::CanCollideWith(const Collider& other) const {
	return isActive_ &&
		other.isActive_ &&
		(collisionMask_ & other.collisionAttribute_) != 0 &&
		(other.collisionMask_ & collisionAttribute_) != 0;
}

Vector3 Collider::GetWorldCenter() const {
	if (!worldMatrix_) {
		return offset_;
	}

	return {
		offset_.x * worldMatrix_->m[0][0] +
		offset_.y * worldMatrix_->m[1][0] +
		offset_.z * worldMatrix_->m[2][0] +
		worldMatrix_->m[3][0],
		offset_.x * worldMatrix_->m[0][1] +
		offset_.y * worldMatrix_->m[1][1] +
		offset_.z * worldMatrix_->m[2][1] +
		worldMatrix_->m[3][1],
		offset_.x * worldMatrix_->m[0][2] +
		offset_.y * worldMatrix_->m[1][2] +
		offset_.z * worldMatrix_->m[2][2] +
		worldMatrix_->m[3][2]
	};
}

float Collider::GetWorldAxisScale(uint32_t axis) const {
	if (!worldMatrix_ || axis >= 3) {
		return 1.0f;
	}

	return Math::Length({
		worldMatrix_->m[axis][0],
		worldMatrix_->m[axis][1],
		worldMatrix_->m[axis][2]
	});
}

float Collider::GetMaxWorldScale() const {
	return (std::max)(
		GetWorldAxisScale(0),
		(std::max)(GetWorldAxisScale(1), GetWorldAxisScale(2))
	);
}

namespace {
	bool SweepFinite(const Vector3& v) {
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}
	bool SweepValid(const ColliderSweepShape& s) {
		if (!SweepFinite(s.center)) return false;
		if (s.type == Collider::Type::Sphere) return std::isfinite(s.radius) && s.radius >= 0.0f;
		if (s.type != Collider::Type::OBB || !SweepFinite(s.halfSize) ||
			s.halfSize.x < 0 || s.halfSize.y < 0 || s.halfSize.z < 0) return false;
		for (size_t i = 0; i < 3; ++i) {
			if (!SweepFinite(s.axis[i]) || std::abs(Math::Dot(s.axis[i], s.axis[i]) - 1.0f) > 0.001f) return false;
			for (size_t j = 0; j < i; ++j) if (std::abs(Math::Dot(s.axis[i], s.axis[j])) > 0.001f) return false;
		}
		return true;
	}
	double SweepDot(const Vector3& a, const Vector3& b) {
		return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z;
	}
	Vector3 SweepUnit(const Vector3& v, const Vector3& fallback = {1, 0, 0}) {
		const double length = std::sqrt(SweepDot(v, v));
		return length > 1e-12 ? Math::Multiply(v, static_cast<float>(1.0 / length)) : fallback;
	}
	// 凸な二次式が初めて0以下になる時刻。doubleで解き、接線接触も含める。
	bool SweepQuadratic(double a, double b, double c, double low, double high, double& time) {
		if ((a * low + b) * low + c <= 0.0) { time = low; return true; }
		if (a == 0.0) {
			if (b >= 0.0) return false;
			const double t = -c / b;
			if (t >= low && t <= high) { time = t; return true; }
			return false;
		}
		const double disc = b * b - 4.0 * a * c;
		if (disc < 0.0) return false;
		const double root = std::sqrt(disc);
		const double q = -0.5 * (b + std::copysign(root, b));
		double t0 = q / a, t1 = q != 0.0 ? c / q : -b / (2.0 * a);
		if (t0 > t1) std::swap(t0, t1);
		const double t = (std::max)(low, t0);
		if (t <= high && t <= t1) { time = t; return true; }
		return false;
	}
	Vector3 SweepBoxSupport(const ColliderSweepShape& box, const Vector3& direction) {
		Vector3 result = box.center;
		const float half[3] = {box.halfSize.x, box.halfSize.y, box.halfSize.z};
		for (size_t i = 0; i < 3; ++i) result = Math::Add(result,
			Math::Multiply(box.axis[i], Math::Dot(direction, box.axis[i]) >= 0 ? half[i] : -half[i]));
		return result;
	}
}

bool CaptureColliderSweepShape(const Collider& collider, ColliderSweepShape& out) {
	ColliderSweepShape shape;
	shape.type = collider.GetType();
	shape.center = collider.GetWorldCenter();
	shape.attribute = collider.GetCollisionAttribute();
	shape.mask = collider.GetCollisionMask();
	if (shape.type == Collider::Type::Sphere) {
		shape.radius = static_cast<const SphereCollider&>(collider).GetRadius();
	} else if (shape.type == Collider::Type::OBB) {
		const auto box = static_cast<const OBBCollider&>(collider).GetOBB();
		shape.center = box.center; shape.axis = box.axis; shape.halfSize = box.halfSize;
	} else return false;
	if (!SweepValid(shape)) return false;
	out = shape;
	return true;
}

ColliderSweepResult SweepColliderTranslation(const ColliderSweepShape& a, const Vector3& deltaA,
	const ColliderSweepShape& b, const Vector3& deltaB, ColliderSweepHit& out) {
	if (!SweepValid(a) || !SweepValid(b) || !SweepFinite(deltaA) || !SweepFinite(deltaB)) return ColliderSweepResult::Invalid;
	if (!(a.mask & b.attribute) || !(b.mask & a.attribute)) return ColliderSweepResult::Miss;
	const Vector3 relative = Math::Subtract(deltaA, deltaB);
	const Vector3 offset = Math::Subtract(a.center, b.center);
	if (!SweepFinite(relative) || !SweepFinite(offset)) return ColliderSweepResult::Invalid;
	double time = 0.0;
	Vector3 normal{};
	if (a.type == Collider::Type::Sphere && b.type == Collider::Type::Sphere) {
		const double radius = double(a.radius) + b.radius;
		if (!SweepQuadratic(SweepDot(relative, relative), 2.0 * SweepDot(offset, relative),
			SweepDot(offset, offset) - radius * radius, 0.0, 1.0, time)) return ColliderSweepResult::Miss;
		normal = SweepUnit(Math::Add(offset, Math::Multiply(relative, static_cast<float>(time))),
			SweepUnit(Math::Multiply(relative, -1.0f)));
	} else if (a.type == Collider::Type::OBB && b.type == Collider::Type::OBB) {
		std::array<Vector3, 15> axes;
		for (size_t i = 0; i < 3; ++i) {
			axes[i] = a.axis[i]; axes[3 + i] = b.axis[i];
			for (size_t j = 0; j < 3; ++j) axes[6 + 3 * i + j] = Math::Cross(a.axis[i], b.axis[j]);
		}
		double enter = 0.0, leave = 1.0, minDepth = (std::numeric_limits<double>::max)();
		Vector3 overlapNormal{1, 0, 0};
		for (const auto& candidate : axes) {
			if (SweepDot(candidate, candidate) < 1e-12) continue;
			const auto axis = SweepUnit(candidate);
			const double radius =
				std::abs(SweepDot(axis, a.axis[0])) * a.halfSize.x + std::abs(SweepDot(axis, a.axis[1])) * a.halfSize.y +
				std::abs(SweepDot(axis, a.axis[2])) * a.halfSize.z + std::abs(SweepDot(axis, b.axis[0])) * b.halfSize.x +
				std::abs(SweepDot(axis, b.axis[1])) * b.halfSize.y + std::abs(SweepDot(axis, b.axis[2])) * b.halfSize.z;
			const double p = SweepDot(offset, axis), v = SweepDot(relative, axis);
			const double depth = radius - std::abs(p);
			if (depth < minDepth) { minDepth = depth; overlapNormal = Math::Multiply(axis, p >= 0 ? 1.0f : -1.0f); }
			if (v == 0.0) { if (depth < 0) return ColliderSweepResult::Miss; continue; }
			double first = (-radius - p) / v, last = (radius - p) / v;
			if (first > last) std::swap(first, last);
			if (first > enter) { enter = first; normal = Math::Multiply(axis, v > 0 ? -1.0f : 1.0f); }
			leave = (std::min)(leave, last);
			if (enter > leave) return ColliderSweepResult::Miss;
		}
		time = enter;
		if (time == 0.0) normal = overlapNormal;
	} else {
		const bool sphereFirst = a.type == Collider::Type::Sphere;
		const auto& sphere = sphereFirst ? a : b;
		const auto& box = sphereFirst ? b : a;
		const auto worldOffset = Math::Subtract(sphere.center, box.center);
		const auto worldDelta = sphereFirst ? relative : Math::Multiply(relative, -1.0f);
		double p[3], v[3];
		const double half[3] = {box.halfSize.x, box.halfSize.y, box.halfSize.z};
		std::vector<double> cuts{0.0, 1.0};
		for (size_t i = 0; i < 3; ++i) {
			p[i] = SweepDot(worldOffset, box.axis[i]); v[i] = SweepDot(worldDelta, box.axis[i]);
			if (v[i] != 0.0) for (double side : {-half[i], half[i]}) {
				const double t = (side - p[i]) / v[i];
				if (t > 0 && t < 1) cuts.push_back(t);
			}
		}
		std::sort(cuts.begin(), cuts.end());
		cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
		bool found = false;
		for (size_t segment = 1; segment < cuts.size(); ++segment) {
			const double mid = (cuts[segment - 1] + cuts[segment]) * 0.5;
			double qa = 0, qb = 0, qc = -double(sphere.radius) * sphere.radius;
			for (size_t i = 0; i < 3; ++i) {
				const double x = p[i] + v[i] * mid;
				if (x >= -half[i] && x <= half[i]) continue;
				const double d = p[i] - (x < 0 ? -half[i] : half[i]);
				qa += v[i] * v[i]; qb += 2.0 * d * v[i]; qc += d * d;
			}
			if (SweepQuadratic(qa, qb, qc, cuts[segment - 1], cuts[segment], time)) { found = true; break; }
		}
		if (!found) return ColliderSweepResult::Miss;
		Vector3 displacement{};
		double depth = (std::numeric_limits<double>::max)();
		Vector3 insideNormal{1, 0, 0};
		for (size_t i = 0; i < 3; ++i) {
			const double x = p[i] + v[i] * time;
			displacement = Math::Add(displacement, Math::Multiply(box.axis[i], static_cast<float>(x - std::clamp(x, -half[i], half[i]))));
			if (half[i] - std::abs(x) < depth) { depth = half[i] - std::abs(x); insideNormal = Math::Multiply(box.axis[i], x >= 0 ? 1.0f : -1.0f); }
		}
		normal = SweepUnit(displacement, insideNormal);
		if (!sphereFirst) normal = Math::Multiply(normal, -1.0f);
	}
	ColliderSweepHit hit;
	hit.fraction = static_cast<float>(std::clamp(time, 0.0, 1.0));
	hit.initialOverlap = time == 0.0;
	hit.normal = normal;
	const Vector3 surfaceA = a.type == Collider::Type::Sphere
		? Math::Subtract(a.center, Math::Multiply(normal, a.radius)) : SweepBoxSupport(a, Math::Multiply(normal, -1.0f));
	const Vector3 surfaceB = b.type == Collider::Type::Sphere
		? Math::Add(b.center, Math::Multiply(normal, b.radius)) : SweepBoxSupport(b, normal);
	hit.position = Math::Add(Math::Add(Math::Multiply(surfaceA, 0.5f), Math::Multiply(surfaceB, 0.5f)),
		Math::Add(Math::Multiply(deltaA, hit.fraction * 0.5f), Math::Multiply(deltaB, hit.fraction * 0.5f)));
	if (!SweepFinite(hit.position) || !SweepFinite(hit.normal)) return ColliderSweepResult::Invalid;
	out = hit;
	return ColliderSweepResult::Hit;
}
