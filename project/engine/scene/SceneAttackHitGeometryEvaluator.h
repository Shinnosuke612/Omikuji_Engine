// 役割: 攻撃判定キーを値だけで検証・評価する。Document/Runtime/編集状態を所有しない。
#pragma once
#include "SceneDocument.h"

namespace SceneAttackHitGeometryEvaluator {
struct Issue { std::string code; size_t key = 0; };
struct Sample { std::string shape; Vector3 offset{}, halfSize{}; float radius = 0; };
// 空trackは固定値互換。非空trackは秒/ローカル値/形状を検証し、原本を変更しない。
bool Validate(const SceneAttackHitWindow& window, float duration, const std::string& shape, Issue& issue);
// 検証済みの非空trackを絶対秒で評価。非有限入力/結果では失敗し、outを変更しない。
bool SampleTrack(const SceneAttackHitGeometryTrack& track, float time, Sample& out, Issue& issue);
}
