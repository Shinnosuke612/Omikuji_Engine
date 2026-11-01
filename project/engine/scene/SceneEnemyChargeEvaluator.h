// 役割: 突進の無衝突経路を評価し、RuntimeとEditorへ同じ時間・距離の値を渡す。
#pragma once

#include <vector>
#include "../math/Easing.h"

struct SceneEnemyChargeSettings;

namespace SceneEnemyChargeEvaluator {

inline constexpr unsigned kSegmentCount = 64;
struct DistanceSample {
	float seconds = 0.0f;
	float distance = 0.0f;
};

// 秒/Scene単位。設定/時刻が非有限、未知曲線、時間幅が表現不能ならfalse。
// 時計・実位置は所有せず、設定を変更しない。出力は成功時だけ変更する。
bool TryGetDuration(const SceneEnemyChargeSettings& settings, float& seconds);
bool EvaluateDistance(const SceneEnemyChargeSettings& settings, float seconds, float& distance);

// 突進全体に固定した64区間の節点と両端を返す（2..65点）。Frameごとの再近似はしない。
// 0 <= begin < end <= Tが必要。失敗時はsamplesを空にし、途中列は渡さない。
bool SampleInterval(const SceneEnemyChargeSettings& settings, float begin, float end,
	std::vector<DistanceSample>& samples);

// 速度図用。解析曲線の微分ではなく、実移動の区分線形経路の速度/平均速度を返す。
bool EvaluateSpeedRatio(const SceneEnemyChargeSettings& settings, float normalizedTime, float& ratio);

}
