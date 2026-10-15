// 役割: Capture/第一描画/終端のGPU確認を境界に、Scene保持と純粋な演出時刻を接続する。
#include "SceneTransitionController.h"
#include "../../engine/scene/SceneManager.h"
#include "../../engine/scene/SceneCatalog.h"
#include <algorithm>
#include <cmath>
#include <utility>

using namespace SceneTransitionAsset;

bool SceneTransitionController::TryPrepareRequest(const PresentedTransitionRequest& request,
    const SceneCatalog& catalog, Audio& audio, Diagnostics& diagnostics) {
    diagnostics.clear();
    if (IsActive() || !catalog.Find(request.destinationSceneId)) {
        diagnostics.push_back({"request", "Transition is busy or destination is not registered."});
        return false;
    }
    std::shared_ptr<const CompiledTransition> candidate;
    if (!TryLoad(request.transitionAssetPath,candidate,diagnostics)) return false;
    std::vector<CueVoice> cues;
    for (const auto& cue : candidate->GetAudioCues()) {
        AudioClipPtr clip;
        if (!cue.clipPath.empty()) {
            std::string error;
            clip = audio.LoadAudioFile(cue.clipPath.c_str(),&error);
            if (!clip) { diagnostics.push_back({"audioCues."+cue.id,error}); return false; }
        }
        cues.push_back({cue,std::move(clip),false});
    }
    FrameSample first;
    if (!TryEvaluate(*candidate,Phase::Cutting,0,first,diagnostics)) return false;
    request_ = request; asset_ = std::move(candidate); cues_ = std::move(cues); sample_ = std::move(first);
    diagnostic_.clear(); time_ = 0; discardDelta_ = true;
    return true;
}

bool SceneTransitionController::BeginPrepared(SceneManager& manager, Diagnostics& diagnostics) {
    diagnostics.clear();
    if (!asset_ || IsActive() || !manager.FindSceneInstance(request_.sourceInstanceId) || !manager.BeginTransitionHold()) {
        diagnostics.push_back({"hold","Source is gone or Scene transition hold is unavailable."});
        return false;
    }
    ownsHold_ = true; ++generation_; stage_ = Stage::AwaitCapture;
    expectedInstance_ = manager.GetActiveSceneInstanceId(); reservation_ = kInvalidSceneInstanceId;
    receipt_.reset(); suspended_ = false;
    return true;
}

void SceneTransitionController::StopVoices(Audio& audio) {
    for (auto& voice : voices_) audio.SoundStop(voice);
    voices_.clear();
}

void SceneTransitionController::Cancel(SceneManager& manager, Audio& audio, const std::string& reason) {
    ++generation_; receipt_.reset();
    if (reservation_ != kInvalidSceneInstanceId) manager.CancelPendingSceneLoad(reservation_);
    if (ownsHold_) manager.EndTransitionHold();
    manager.FinishPresentedTransitionRequest();
    StopVoices(audio);
    ownsHold_ = false; stage_ = Stage::Idle; time_ = 0;
    reservation_ = expectedInstance_ = kInvalidSceneInstanceId;
    asset_.reset(); cues_.clear(); sample_ = {}; suspended_ = false; discardDelta_ = true;
    if (!reason.empty()) diagnostic_ = reason;
}

void SceneTransitionController::FireCues(Audio& audio, Phase phase, double previous, double current, bool start) {
    voices_.erase(std::remove_if(voices_.begin(),voices_.end(),[&](auto& voice) {
        if (audio.IsPlaying(voice)) return false;
        audio.SoundStop(voice); return true;
    }),voices_.end());
    for (auto& value : cues_) {
        const auto& cue = value.cue;
        if (value.fired || cue.phase != phase || !(start ? cue.time == 0 : cue.time > previous && cue.time <= current)) continue;
        value.fired = true;
        if (!value.clip) continue;
        AudioPlaybackOptions options;
        options.bus = AudioBus::SFX; options.volume = cue.volume; options.pitch = cue.pitch;
        options.loop = false; options.trackNaturalCompletion = false;
        auto voice = audio.PlayAudioClip(value.clip,options);
        if (voice.IsValid()) voices_.push_back(voice);
        else diagnostic_ = "Transition cue playback failed: " + cue.id;
    }
}

bool SceneTransitionController::Evaluate(SceneManager& manager, Audio& audio) {
    Diagnostics diagnostics;
    const Phase phase = (stage_ == Stage::Collapse || stage_ == Stage::FinishPresented) ? Phase::Collapse : Phase::Cutting;
    if (TryEvaluate(*asset_,phase,static_cast<float>(time_),sample_,diagnostics)) return true;
    Cancel(manager,audio,diagnostics.empty() ? "Transition sample failed." : diagnostics.front().message);
    return false;
}

void SceneTransitionController::NotifyDrawRecorded(std::uint64_t serial, SceneInstanceId instance) {
    if (IsActive() && instance == expectedInstance_)
        receipt_ = Receipt{generation_,serial,stage_,instance,sample_.localTime};
}
void SceneTransitionController::DiscardDrawReceipt() { receipt_.reset(); discardDelta_ = true; }

void SceneTransitionController::UpdateBeforeScene(SceneManager& manager, Audio& audio,
    float delta, bool suspended, std::uint64_t completedSerial) {
    if (!IsActive()) return;
    if (suspended != suspended_) {
        for (auto& voice : voices_) { if (suspended) audio.Pause(voice); else audio.Resume(voice); }
        suspended_ = suspended; discardDelta_ = true;
    }
    if (suspended) { receipt_.reset(); return; }
    if (manager.GetActiveSceneInstanceId() != expectedInstance_) {
        Cancel(manager,audio,"Active Scene changed outside the transition."); return;
    }
    if (!receipt_ || receipt_->serial > completedSerial || receipt_->generation != generation_ ||
        receipt_->stage != stage_ || receipt_->instance != expectedInstance_ || receipt_->time != sample_.localTime) return;
    receipt_.reset();
    if (stage_ == Stage::AwaitCapture) {
        stage_ = Stage::Cutting; time_ = 0; discardDelta_ = true;
        FireCues(audio,Phase::Cutting,0,0,true); Evaluate(manager,audio); return;
    }
    if (stage_ == Stage::Cutting && time_ >= asset_->GetAsset().cutPhaseDuration) {
        // 終端を実際に表示してから一回だけ予約。同じScene IDでも新Instanceを追跡する。
        reservation_ = manager.LoadScene(request_.destinationSceneId,SceneLoadMode::Single);
        if (reservation_ == kInvalidSceneInstanceId) { Cancel(manager,audio,"Scene load reservation failed."); return; }
        stage_ = Stage::AwaitActivation; discardDelta_ = true; return;
    }
    if (stage_ == Stage::AwaitDestinationFrame) {
        stage_ = Stage::Collapse; time_ = 0; discardDelta_ = true;
        FireCues(audio,Phase::Collapse,0,0,true); Evaluate(manager,audio); return;
    }
    if (stage_ == Stage::FinishPresented) { Cancel(manager,audio); return; }
    if (stage_ != Stage::Cutting && stage_ != Stage::Collapse) return;
    if (!std::isfinite(delta) || delta < 0) { Cancel(manager,audio,"Invalid transition delta."); return; }
    const double previous = time_;
    const Phase phase = stage_ == Stage::Cutting ? Phase::Cutting : Phase::Collapse;
    const double end = phase == Phase::Cutting ? asset_->GetAsset().cutPhaseDuration : asset_->GetAsset().collapseDuration;
    time_ = (std::min)(end,time_ + (discardDelta_ ? 0.0 : static_cast<double>(delta)));
    discardDelta_ = false;
    FireCues(audio,phase,previous,time_,false);
    if (phase == Phase::Collapse && time_ >= end) stage_ = Stage::FinishPresented;
    Evaluate(manager,audio);
}

void SceneTransitionController::UpdateAfterScene(SceneManager& manager, Audio& audio) {
    if (stage_ != Stage::AwaitActivation) return;
    SceneLoadOutcome outcome;
    if (!manager.ConsumeLoadOutcome(reservation_,outcome)) return;
    if (outcome.status != SceneLoadStatus::Activated || manager.GetActiveSceneInstanceId() != reservation_) {
        Cancel(manager,audio,"Destination load failed: "+outcome.diagnostic); return;
    }
    expectedInstance_ = reservation_; stage_ = Stage::AwaitDestinationFrame;
    receipt_.reset(); discardDelta_ = true;
}
