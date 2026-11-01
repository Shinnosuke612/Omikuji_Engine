// 役割: 突進全体の固定節点から距離と速度を求め、Frame内の経路を採取する。
#include "SceneEnemyChargeEvaluator.h"
#include "SceneDocument.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace SceneEnemyChargeEvaluator {
namespace {
	float KnotTime(float duration, unsigned index) {
		return static_cast<float>(double(duration) * index / kSegmentCount);
	}
	// 同じfloatの節点を距離評価/採取の両方で使い、節点上のFrame端点を二重登録しない。
	bool KnotDistances(Easing::Curve curve, unsigned segment, float& begin, float& end) {
		return Easing::Evaluate(curve, float(segment) / kSegmentCount, begin) &&
			Easing::Evaluate(curve, float(segment + 1) / kSegmentCount, end);
	}
}

bool TryGetDuration(const SceneEnemyChargeSettings& settings, float& seconds) {
	Easing::Curve curve;
	if (!Easing::TryParse(settings.movementEasing, curve) || !std::isfinite(settings.speed) ||
		!std::isfinite(settings.distance) || settings.speed <= 0 || settings.distance <= 0) return false;
	const float duration = settings.distance / settings.speed;
	if (!std::isfinite(duration) || duration <= 0) return false;
	seconds = duration;
	return true;
}

bool EvaluateDistance(const SceneEnemyChargeSettings& settings, float seconds, float& distance) {
	float duration;
	Easing::Curve curve;
	if (!std::isfinite(seconds) || !TryGetDuration(settings, duration) || !Easing::TryParse(settings.movementEasing, curve)) return false;
	const float time = std::clamp(seconds, 0.0f, duration);
	const double position = double(time) / duration * kSegmentCount;
	unsigned segment = (std::min)(static_cast<unsigned>(position), kSegmentCount - 1);
	if (segment > 0 && time < KnotTime(duration, segment)) --segment;
	if (segment + 1 < kSegmentCount && time >= KnotTime(duration, segment + 1)) ++segment;
	const float beginTime = KnotTime(duration, segment), endTime = KnotTime(duration, segment + 1);
	if (!(endTime > beginTime)) return false;
	float begin, end;
	if (!KnotDistances(curve, segment, begin, end)) return false;
	const double rate = (double(time) - beginTime) / (double(endTime) - beginTime);
	const float result = static_cast<float>(settings.distance * (double(begin) + (double(end) - begin) * rate));
	if (!std::isfinite(result)) return false;
	distance = result;
	return true;
}

bool SampleInterval(const SceneEnemyChargeSettings& settings, float begin, float end,
	std::vector<DistanceSample>& samples) {
	samples.clear();
	float duration, beginDistance, endDistance;
	if (!TryGetDuration(settings, duration) || !std::isfinite(begin) || !std::isfinite(end) ||
		begin < 0 || begin >= end || end > duration ||
		!EvaluateDistance(settings, begin, beginDistance) || !EvaluateDistance(settings, end, endDistance)) return false;
	std::vector<DistanceSample> result{{begin, beginDistance}};
	result.reserve(kSegmentCount + 1);
	float previousKnot = 0;
	for (unsigned i = 1; i < kSegmentCount; ++i) {
		const float time = KnotTime(duration, i);
		if (time <= previousKnot || time >= duration) return false;
		previousKnot = time;
		if (time <= begin || time >= end) continue;
		float distance;
		if (!EvaluateDistance(settings, time, distance)) return false;
		result.push_back({time, distance});
	}
	result.push_back({end, endDistance});
	samples = std::move(result);
	return true;
}

bool EvaluateSpeedRatio(const SceneEnemyChargeSettings& settings, float normalizedTime, float& ratio) {
	float duration;
	if (!std::isfinite(normalizedTime) || !TryGetDuration(settings, duration)) return false;
	const float position = std::clamp(normalizedTime, 0.0f, 1.0f) * kSegmentCount;
	const unsigned segment = (std::min)(static_cast<unsigned>(position), kSegmentCount - 1);
	const float beginTime = KnotTime(duration, segment), endTime = KnotTime(duration, segment + 1);
	float begin, end;
	if (!(endTime > beginTime) || !EvaluateDistance(settings, beginTime, begin) || !EvaluateDistance(settings, endTime, end)) return false;
	const float result = static_cast<float>((double(end) - begin) / (double(endTime) - beginTime) / settings.speed);
	if (!std::isfinite(result)) return false;
	ratio = result;
	return true;
}

}
