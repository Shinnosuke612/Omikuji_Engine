// 役割: シーン内のCameraPathを実行時の補間可能なカメラ経路へ変換する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../math/Transform.h"

class Camera;
class SceneDocument;
struct SceneComponent;
struct SceneEntity;

class CameraPathRuntime {
public:
	struct PointSample {
		Transform transform{};
		float durationToNext = 1.0f;
		std::string easingToNext = "SmoothStep";
		uint64_t entityId = 0;
	};

	void Play(
		const SceneDocument& document,
		const SceneEntity& pathEntity,
		const SceneComponent& pathComponent,
		const Camera& currentCamera
	);
	void Stop();
	bool IsPlaying() const { return phase_ != Phase::Finished; }
	bool ConsumeFinishedThisFrame() {
		const bool finished = finishedThisFrame_;
		finishedThisFrame_ = false;
		return finished;
	}

	void Update(float deltaTime, Camera& camera);
	// 開始済み経路だけを縮小秒で進め、段階越えの余り時間と終端姿勢を反映する。
	// 不正/非正deltaは状態不変。通常のFinished通知は生成しない。
	void UpdateDeathContinuation(float worldDelta, Camera& camera);

	const std::vector<PointSample>& GetPoints() const { return points_; }
	const Transform& GetCurrentTransform() const { return currentTransform_; }
	bool HasCurrentTransform() const { return hasCurrentTransform_; }

private:
	enum class Phase {
		Enter,
		Path,
		Exit,
		Finished
	};

	void ApplyTransform(Camera& camera, const Transform& transform);
	Transform EvaluatePath(float elapsed) const;
	float GetPathDuration() const;

	Phase phase_ = Phase::Finished;
	float elapsed_ = 0.0f;
	float pathElapsed_ = 0.0f;
	Transform startTransform_{};
	Transform returnTransform_{};
	Transform currentTransform_{};
	bool hasCurrentTransform_ = false;
	float enterDuration_ = 0.5f;
	float exitDuration_ = 0.5f;
	std::string enterEasing_ = "SmoothStep";
	std::string exitEasing_ = "SmoothStep";
	bool returnToPreviousCamera_ = true;
	std::string interpolation_ = "Linear";
	std::vector<PointSample> points_;
	bool finishedThisFrame_ = false;
};
