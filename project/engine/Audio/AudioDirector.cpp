// 役割: AudioのStreaming Handleだけを操作し、Sceneデータを保持せずBGM遷移を進める。
#include "AudioDirector.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

std::string AudioDirector::NormalizePath(const std::string& path) {
	std::string normalized = std::filesystem::path(path).lexically_normal().generic_string();
	std::transform(
		normalized.begin(),
		normalized.end(),
		normalized.begin(),
		[](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		}
	);
	return normalized;
}

bool AudioDirector::Matches(
	const std::optional<Track>& track,
	uint64_t sceneOwnerId,
	uint64_t componentKey
) {
	return track && track->request.sceneOwnerId == sceneOwnerId &&
		track->request.componentKey == componentKey;
}

void AudioDirector::ClearFinishedSignals(
	uint64_t sceneOwnerId,
	uint64_t componentKey
) {
	std::erase_if(
		finishedSignals_,
		[sceneOwnerId, componentKey](const FinishedSignal& signal) {
			return signal.sceneOwnerId == sceneOwnerId &&
				(componentKey == 0 || signal.componentKey == componentKey);
		}
	);
}

void AudioDirector::StopTrack(std::optional<Track>& track) {
	if (!track) {
		return;
	}
	audio_.SoundStop(track->handle);
	track.reset();
}

void AudioDirector::BeginFade(float duration) {
	fadeElapsed_ = 0.0f;
	fadeDuration_ = (std::max)(duration, 0.0f);
	fading_ = true;
}

bool AudioDirector::Resolve(const PersistentBgmRequest& request) {
	transitionHold_ = false;
	transitionOwnerId_ = 0;
	const std::string normalizedPath = NormalizePath(request.clipPath);
	if (normalizedPath.empty()) {
		ResolveNone(request.sceneOwnerId);
		return false;
	}

	if (current_ && current_->normalizedPath == normalizedPath) {
		ClearFinishedSignals(current_->request.sceneOwnerId);
		current_->request = request;
		audio_.SetPlaybackProperties(
			current_->handle,
			request.options.volume,
			request.options.pitch,
			request.options.loop
		);
		StopTrack(pendingIncoming_);
		return current_->handle.IsValid();
	}
	if (pendingIncoming_ && pendingIncoming_->normalizedPath == normalizedPath) {
		ClearFinishedSignals(pendingIncoming_->request.sceneOwnerId);
		pendingIncoming_->request = request;
		audio_.SetPlaybackProperties(
			pendingIncoming_->handle,
			request.options.volume,
			request.options.pitch,
			request.options.loop
		);
		return pendingIncoming_->handle.IsValid();
	}

	// 新しい要求を優先し、未開始の古いincomingはVoice上限へ数えないよう停止する。
	StopTrack(pendingIncoming_);
	if (outgoing_) {
		StopTrack(outgoing_);
	}
	if (current_) {
		// incoming失敗時も新Sceneの終了で確実に停止できるよう、fallbackのownerだけを付け替える。
		current_->request.sceneOwnerId = request.sceneOwnerId;
		current_->request.componentKey = request.componentKey;
	}
	std::string error;
	Audio::PlaybackHandle handle = audio_.PlayAudioStream(
		request.clipPath.c_str(),
		request.options,
		&error
	);
	if (!handle.IsValid()) {
		return false;
	}
	audio_.SetPlaybackFadeGain(handle, 0.0f);
	pendingIncoming_ = Track{ request, normalizedPath, handle };
	return true;
}

void AudioDirector::ResolveNone(uint64_t nextSceneOwnerId) {
	transitionHold_ = false;
	transitionOwnerId_ = 0;
	StopTrack(pendingIncoming_);
	if (outgoing_) {
		StopTrack(outgoing_);
	}
	if (!current_) {
		return;
	}
	ClearFinishedSignals(current_->request.sceneOwnerId);
	current_->request.sceneOwnerId = nextSceneOwnerId;
	current_->request.componentKey = 0;
	const float fadeSeconds = current_->request.fadeSeconds;
	outgoing_ = std::move(current_);
	current_.reset();
	BeginFade(fadeSeconds);
}

void AudioDirector::BeginSceneTransition(uint64_t sceneOwnerId) {
	ClearFinishedSignals(sceneOwnerId);
	if (
		(current_ && current_->request.sceneOwnerId == sceneOwnerId) ||
		(pendingIncoming_ && pendingIncoming_->request.sceneOwnerId == sceneOwnerId)
	) {
		transitionHold_ = true;
		transitionOwnerId_ = sceneOwnerId;
	}
}

void AudioDirector::ReleaseOwner(uint64_t sceneOwnerId) {
	ClearFinishedSignals(sceneOwnerId);
	if (transitionHold_ && transitionOwnerId_ == sceneOwnerId) {
		return;
	}
	if (current_ && current_->request.sceneOwnerId == sceneOwnerId) {
		StopTrack(current_);
	}
	if (pendingIncoming_ && pendingIncoming_->request.sceneOwnerId == sceneOwnerId) {
		StopTrack(pendingIncoming_);
	}
	if (outgoing_ && outgoing_->request.sceneOwnerId == sceneOwnerId) {
		StopTrack(outgoing_);
	}
	fading_ = false;
}

void AudioDirector::Stop(uint64_t sceneOwnerId, uint64_t componentKey) {
	ClearFinishedSignals(sceneOwnerId, componentKey);
	if (Matches(current_, sceneOwnerId, componentKey)) {
		StopTrack(current_);
	}
	if (Matches(pendingIncoming_, sceneOwnerId, componentKey)) {
		StopTrack(pendingIncoming_);
	}
	if (Matches(outgoing_, sceneOwnerId, componentKey)) {
		StopTrack(outgoing_);
	}
	fading_ = false;
}

void AudioDirector::Pause(uint64_t sceneOwnerId, uint64_t componentKey) {
	if (Matches(current_, sceneOwnerId, componentKey)) {
		audio_.Pause(current_->handle);
	}
}

void AudioDirector::Resume(uint64_t sceneOwnerId, uint64_t componentKey) {
	if (Matches(current_, sceneOwnerId, componentKey)) {
		audio_.Resume(current_->handle);
	}
}

bool AudioDirector::ConsumeFinished(
	uint64_t sceneOwnerId,
	uint64_t componentKey
) {
	const auto found = std::find_if(
		finishedSignals_.begin(),
		finishedSignals_.end(),
		[sceneOwnerId, componentKey](const FinishedSignal& signal) {
			return signal.sceneOwnerId == sceneOwnerId &&
				signal.componentKey == componentKey;
		}
	);
	if (found == finishedSignals_.end()) {
		return false;
	}
	finishedSignals_.erase(found);
	return true;
}

void AudioDirector::Update(float realDeltaTime) {
	if (pendingIncoming_) {
		if (!audio_.IsPlaying(pendingIncoming_->handle)) {
			StopTrack(pendingIncoming_);
		} else if (audio_.IsPlaybackStarted(pendingIncoming_->handle)) {
			if (outgoing_) {
				StopTrack(outgoing_);
			}
			outgoing_ = std::move(current_);
			current_ = std::move(pendingIncoming_);
			pendingIncoming_.reset();
			BeginFade(current_->request.fadeSeconds);
		}
	}

	if (current_ && !audio_.IsPlaying(current_->handle)) {
		if (
			!transitionHold_ &&
			audio_.ConsumeNaturalCompletion(current_->handle)
		) {
			finishedSignals_.push_back({
				current_->request.sceneOwnerId,
				current_->request.componentKey
			});
		} else {
			// errorまたはtransition holdの完了候補はSceneへ渡さない。
			audio_.SoundStop(current_->handle);
		}
		current_.reset();
	}
	if (outgoing_ && !audio_.IsPlaying(outgoing_->handle)) {
		StopTrack(outgoing_);
	}
	if (!fading_) {
		return;
	}

	fadeElapsed_ += (std::max)(realDeltaTime, 0.0f);
	const float progress = fadeDuration_ <= 0.0f
		? 1.0f
		: (std::clamp)(fadeElapsed_ / fadeDuration_, 0.0f, 1.0f);
	if (current_) {
		audio_.SetPlaybackFadeGain(current_->handle, progress);
	}
	if (outgoing_) {
		audio_.SetPlaybackFadeGain(outgoing_->handle, 1.0f - progress);
	}
	if (progress < 1.0f) {
		return;
	}
	StopTrack(outgoing_);
	if (current_) {
		audio_.SetPlaybackFadeGain(current_->handle, 1.0f);
	}
	fading_ = false;
}

void AudioDirector::Reset() {
	transitionHold_ = false;
	transitionOwnerId_ = 0;
	fading_ = false;
	StopTrack(pendingIncoming_);
	StopTrack(outgoing_);
	StopTrack(current_);
	finishedSignals_.clear();
}
