// 役割: Persistent 2D BGMのScene間所有、same-Clip継続、Crossfadeを管理する。
#pragma once

#include "Audio.h"

#include <optional>
#include <string>
#include <vector>

class AudioDirector {
public:
	explicit AudioDirector(Audio& audio) : audio_(audio) {}

	// 要求の同期受付結果。Streamingの実再生開始/完了の保証ではない。
	bool Resolve(const PersistentBgmRequest& request);
	void ResolveNone(uint64_t nextSceneOwnerId);
	void BeginSceneTransition(uint64_t sceneOwnerId);
	void ReleaseOwner(uint64_t sceneOwnerId);
	void Stop(uint64_t sceneOwnerId, uint64_t componentKey);
	void Pause(uint64_t sceneOwnerId, uint64_t componentKey);
	void Resume(uint64_t sceneOwnerId, uint64_t componentKey);
	// SceneAudioSystemがcurrent Persistent BGMの自然終了を一回だけ回収する。
	bool ConsumeFinished(uint64_t sceneOwnerId, uint64_t componentKey);
	void Update(float realDeltaTime);
	void Reset();

private:
	struct Track {
		PersistentBgmRequest request;
		std::string normalizedPath;
		Audio::PlaybackHandle handle;
	};

	struct FinishedSignal {
		uint64_t sceneOwnerId = 0;
		uint64_t componentKey = 0;
	};

	static std::string NormalizePath(const std::string& path);
	static bool Matches(
		const std::optional<Track>& track,
		uint64_t sceneOwnerId,
		uint64_t componentKey
	);
	void ClearFinishedSignals(uint64_t sceneOwnerId, uint64_t componentKey = 0);
	void StopTrack(std::optional<Track>& track);
	void BeginFade(float duration);

	Audio& audio_;
	std::optional<Track> current_;
	std::optional<Track> outgoing_;
	std::optional<Track> pendingIncoming_;
	float fadeElapsed_ = 0.0f;
	float fadeDuration_ = 0.0f;
	bool fading_ = false;
	bool transitionHold_ = false;
	uint64_t transitionOwnerId_ = 0;
	std::vector<FinishedSignal> finishedSignals_;
};
