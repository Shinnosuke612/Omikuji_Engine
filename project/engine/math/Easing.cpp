// 役割: 5種のイージングの識別と、正規化した距離の純粋計算を実装する。
#include "Easing.h"

#include <algorithm>
#include <cmath>

namespace Easing {

bool TryParse(std::string_view name, Curve& curve) {
	if (name == "Linear") curve = Curve::Linear;
	else if (name == "EaseIn") curve = Curve::EaseIn;
	else if (name == "EaseOut") curve = Curve::EaseOut;
	else if (name == "EaseInOut") curve = Curve::EaseInOut;
	else if (name == "SmoothStep") curve = Curve::SmoothStep;
	else return false;
	return true;
}

bool Evaluate(Curve curve, float normalizedTime, float& distance) {
	if (!std::isfinite(normalizedTime)) return false;
	const float u = std::clamp(normalizedTime, 0.0f, 1.0f);
	const float remaining = 1.0f - u;
	switch (curve) {
	case Curve::Linear: distance = u; break;
	case Curve::EaseIn: distance = u * u * u; break;
	case Curve::EaseOut: distance = 1.0f - remaining * remaining * remaining; break;
	case Curve::EaseInOut: distance = u < 0.5f ? 4.0f * u * u * u : 1.0f - 4.0f * remaining * remaining * remaining; break;
	case Curve::SmoothStep: distance = u * u * (3.0f - 2.0f * u); break;
	default: return false;
	}
	return true;
}

}
