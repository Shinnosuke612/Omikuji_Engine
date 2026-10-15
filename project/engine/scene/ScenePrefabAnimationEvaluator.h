// 役割: Prefab Animation Clipを指定時刻でScene Documentへ適用する。
#pragma once

#include <cstdint>

class SceneDocument;
struct ScenePrefabAnimationClip;
struct SceneAnimationTrack;
struct Vector3;

namespace ScenePrefabAnimationEvaluator {
	// LocalRotation/EulerContinuousを絶対時刻（秒）でsampleし、周回付きEuler radianを返す。
	// 空Track・非有限値・不正な時刻順・別方式ではfalse。失敗時valueは変更しない。
	// Document/Trackを書き換えず、端点外は端点値。Editorの不足キー補完も同じ評価を使う。
	bool TrySampleContinuousEuler(
		const SceneAnimationTrack& track,
		float time,
		Vector3& value
	);
	// 解決したtargetのTRS/Activeを変更する。Euler評価失敗時はその回転を変更しない。
	// 未知の回転方式は旧Slerpへfallbackし、診断はSceneValidatorが担当する。
	void ApplyClip(
		SceneDocument& document,
		uint64_t ownerEntityId,
		const ScenePrefabAnimationClip& clip,
		float time
	);
}
