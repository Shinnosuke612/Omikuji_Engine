// 役割: Sceneや時計に依存せず、正規化した距離のイージングを評価する。
#pragma once

#include <string_view>

namespace Easing {

enum class Curve { Linear, EaseIn, EaseOut, EaseInOut, SmoothStep };

// 未知の保存名はfalse。呼出側で原文を保持し、別曲線へ暗黙変換しない。
bool TryParse(std::string_view name, Curve& curve);

// 有限な時刻を0..1へ制限する。未知enum/非有限入力ではfalse、出力は変更しない。
bool Evaluate(Curve curve, float normalizedTime, float& distance);

}
