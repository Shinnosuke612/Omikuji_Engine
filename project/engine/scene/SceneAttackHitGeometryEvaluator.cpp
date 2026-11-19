// 役割: Windowに保存した判定のLinear/Hold補間をRuntimeとPreviewへ共通提供する。
#include "SceneAttackHitGeometryEvaluator.h"
#include <algorithm>
#include <cmath>

namespace SceneAttackHitGeometryEvaluator {
namespace {
bool Finite(const Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool Positive(const Vector3& v) { return Finite(v) && v.x >= .001f && v.y >= .001f && v.z >= .001f; }
}
bool Validate(const SceneAttackHitWindow& w, float duration, const std::string& shape, Issue& issue) {
	issue = {};
	if (w.geometryTrack.keys.empty()) return true;
	auto fail = [&](const char* code) { issue.code = code; return false; };
	if (w.payloadSource != "HitBox") return fail("Legacy");
	if (!std::isfinite(duration) || duration < 0) return fail("Time");
	if ((shape != "Box" && shape != "Sphere") || w.geometryTrack.shape != shape) return fail("Shape");
	float previous = -1;
	for (size_t i = 0; i < w.geometryTrack.keys.size(); ++i) {
		issue.key = i;
		const auto& k = w.geometryTrack.keys[i];
		if (!std::isfinite(k.time) || k.time < 0 || k.time > duration || k.time <= previous) return fail("Time");
		if (!Finite(k.offset) || (shape == "Box" ? !Positive(k.halfSize) : !std::isfinite(k.radius) || k.radius < .001f)) return fail("Value");
		if (k.interpolation != "Linear" && k.interpolation != "Hold") return fail("Interpolation");
		previous = k.time;
	}
	issue = {};
	return true;
}
bool SampleTrack(const SceneAttackHitGeometryTrack& track, float time, Sample& out, Issue& issue) {
	issue = {};
	if (track.keys.empty() || !std::isfinite(time)) { issue.code = "Time"; return false; }
	const auto right = std::upper_bound(track.keys.begin(), track.keys.end(), time,
		[](float t, const SceneAttackHitGeometryKey& k) { return t < k.time; });
	const auto& left = right == track.keys.begin() ? track.keys.front() : *(right - 1);
	Sample result{track.shape, left.offset, left.halfSize, left.radius};
	if (right != track.keys.begin() && right != track.keys.end() && left.interpolation == "Linear") {
		// double中間値により、有限な両端の差がfloatでoverflowすることを避ける。
		const double u = (static_cast<double>(time) - left.time) / (static_cast<double>(right->time) - left.time);
		auto blend = [u](float a, float b) { return static_cast<float>((1.0 - u) * a + u * b); };
		auto vector = [&](Vector3 a, Vector3 b) { return Vector3{blend(a.x,b.x),blend(a.y,b.y),blend(a.z,b.z)}; };
		result.offset = vector(left.offset, right->offset);
		if (track.shape == "Box") result.halfSize = vector(left.halfSize, right->halfSize);
		else result.radius = blend(left.radius, right->radius);
	}
	if (!Finite(result.offset) || (track.shape == "Box" ? !Positive(result.halfSize) : !std::isfinite(result.radius) || result.radius < .001f)) {
		issue.code = "Value"; return false;
	}
	out = result;
	return true;
}
}
