// 役割: Capture/第一描画/終端のGPU確認を境界に、Scene保持と純粋な演出時刻を接続する。
#include "SceneTransitionController.h"
#include "../../engine/scene/SceneManager.h"
#include "../../engine/scene/SceneCatalog.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <type_traits>
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
    return TryPrepareDecodedRequest(request, std::move(candidate), catalog, audio, diagnostics);
}

bool SceneTransitionController::TryPrepareDecodedRequest(const PresentedTransitionRequest& request,
    std::shared_ptr<const CompiledTransition> candidate, const SceneCatalog& catalog, Audio& audio, Diagnostics& diagnostics) {
    diagnostics.clear();
    if (IsActive() || !catalog.Find(request.destinationSceneId)) {
        diagnostics.push_back({"request", "Transition is busy or destination is not registered."});
        return false;
    }
    if (!candidate) {
        diagnostics.push_back({"asset", "Decoded V1 transition is missing."});
        return false;
    }
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
    loadLinked_.reset(); // 保留出口/receipt/identityを失効。実JobとGPUの回収はGameが担当する。
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
    if (!loadLinked_ && IsActive() && instance == expectedInstance_)
        receipt_ = Receipt{generation_,serial,stage_,instance,sample_.localTime};
}
void SceneTransitionController::DiscardDrawReceipt() {
    if (loadLinked_) { DiscardLoadLinkedPresentation(); return; }
    receipt_.reset(); discardDelta_ = true;
}

void SceneTransitionController::UpdateBeforeScene(SceneManager& manager, Audio& audio,
    float delta, bool suspended, std::uint64_t completedSerial) {
    if (loadLinked_) return; // V2は専用の実状態/可視通知入口で進める。
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
    if (loadLinked_) return;
    if (stage_ != Stage::AwaitActivation) return;
    SceneLoadOutcome outcome;
    if (!manager.ConsumeLoadOutcome(reservation_,outcome)) return;
    if (outcome.status != SceneLoadStatus::Activated || manager.GetActiveSceneInstanceId() != reservation_) {
        Cancel(manager,audio,"Destination load failed: "+outcome.diagnostic); return;
    }
    expectedInstance_ = reservation_; stage_ = Stage::AwaitDestinationFrame;
    receipt_.reset(); discardDelta_ = true;
}

namespace {
using LinkedCode = SceneTransitionAsset::LoadLinkedDiagnosticCode;
using LinkedPhase = SceneTransitionAsset::LoadLinkedEvaluationPhase;
using LinkedController = SceneTransitionController;
using LinkedStage = LinkedController::LoadLinkedStage;
using LinkedLoadState = LinkedController::LoadLinkedLoadState;
struct LinkedProgressFailure { LinkedCode code; const char* field; const char* detail; };
void LinkedCheck(bool condition, LinkedCode code, const char* field, const char* detail) {
    if (!condition) throw LinkedProgressFailure{code, field, detail};
}
bool SameLinkedIdentity(const LinkedController::LoadLinkedIdentity& a,
    const LinkedController::LoadLinkedIdentity& b) noexcept {
    return a.ownerId == b.ownerId && a.generation == b.generation &&
        a.requestId == b.requestId && a.settingsRevision == b.settingsRevision;
}
bool SameLinkedBinding(const LinkedController::LoadLinkedSceneBinding& a,
    const LinkedController::LoadLinkedSceneBinding& b) noexcept {
    return SameLinkedIdentity(a.identity, b.identity) && a.tokenId == b.tokenId && a.tokenGeneration == b.tokenGeneration;
}
bool KnownLinkedLoadState(LinkedLoadState state) noexcept {
    switch (state) {
    case LinkedLoadState::NotStarted:
    case LinkedLoadState::Preparing:
    case LinkedLoadState::Ready:
    case LinkedLoadState::Failed:
    case LinkedLoadState::Cancelled:
        return true;
    default: return false;
    }
}
void ValidateLinkedGenerated(const LoadLinkedGenerated& generated) {
    const auto& settings = generated.Settings();
    LinkedCheck(settings.minimumCuts >= 1 && settings.minimumCuts <= settings.maximumCuts &&
        settings.maximumCuts <= MaxCuts && generated.Cuts().size() == settings.maximumCuts &&
        generated.CutSegments().size() == settings.maximumCuts,
        LinkedCode::InternalFailure, "loadLinked.generated", "生成物の候補件数が不正です");
    for (std::size_t n = settings.minimumCuts; n <= settings.maximumCuts; ++n) {
        const auto* prefix = generated.FindPrefix(n);
        LinkedCheck(prefix && prefix->count == n && !prefix->geometry.fragments.empty() &&
            prefix->geometry.fragments.size() <= MaxFragments && !prefix->geometry.binding.empty() &&
            !prefix->geometry.vertices.empty() && prefix->geometry.vertices.size() <= MaxVertices &&
            !prefix->geometry.indices.empty() && prefix->geometry.indices.size() <= MaxIndices &&
            prefix->geometry.indices.size() % 3 == 0 &&
            prefix->geometry.cutSegments.size() == n && prefix->motions.size() == prefix->geometry.fragments.size(),
            LinkedCode::InternalFailure, "loadLinked.generated.prefix", "完成prefixの件数が不正です");
        for (std::size_t i = 0; i < prefix->motions.size(); ++i)
            LinkedCheck(prefix->motions[i].fragmentId == prefix->geometry.fragments[i].fragmentId &&
                (i == 0 || prefix->geometry.fragments[i - 1].fragmentId < prefix->geometry.fragments[i].fragmentId),
                LinkedCode::InternalFailure, "loadLinked.generated.prefix", "完成prefixのID対応が不正です");
    }
}
} // namespace

bool SceneTransitionController::TryBeginLoadLinked(const LoadLinkedStart& start, SceneManager& manager,
    LoadLinkedDiagnostics& diagnostics) {
    diagnostics.clear();
    try {
        // V1の未開始prepared assetも保全し、別modeの開始で上書きしない。
        LinkedCheck(!IsActive() && !asset_, LinkedCode::InvalidType, "loadLinked.begin", "遷移またはV1準備値が使用中です");
        LinkedCheck(start.generated && start.ownerId != 0 && start.generation != 0 &&
            start.generated->RequestId() != 0, LinkedCode::InvalidType, "loadLinked.identity", "生成物または必須IDがありません");
        LinkedCheck(start.request.sourceInstanceId != kInvalidSceneInstanceId &&
            manager.FindSceneInstance(start.request.sourceInstanceId) &&
            manager.GetActiveSceneInstanceId() == start.request.sourceInstanceId &&
            !start.request.destinationSceneId.empty(), LinkedCode::ExternalChange,
            "loadLinked.request", "要求元または遷移先が無効です");
        // destinationのcatalog登録とGPU準備はGameが開始前に保証する。
        if (!TryValidateLoadLinked(start.generated->Settings(), diagnostics)) return false;
        ValidateLinkedGenerated(*start.generated);
        LoadLinkedRuntime candidate;
        candidate.identity = {start.ownerId, start.generation, start.generated->RequestId(), start.generated->SettingsRevision()};
        candidate.generated = start.generated;
        candidate.expectedInstance = start.request.sourceInstanceId;
        candidate.prepare = {candidate.identity, start.request};
        candidate.frame.input.expectedRequestId = candidate.identity.requestId;
        candidate.frame.input.expectedSettingsRevision = candidate.identity.settingsRevision;
        candidate.frame.input.presentationGeneration = candidate.identity.generation;
        LoadLinkedFrameSample sample;
        if (!TryEvaluateLoadLinked(candidate.generated, candidate.frame.input, sample, diagnostics)) return false;
        candidate.frame.sample = std::make_shared<const LoadLinkedFrameSample>(std::move(sample));
        // 全コピー/評価/共有所有の確保後にholdを取得。成功後の交換は割当を伴わない。
        LinkedCheck(manager.BeginTransitionHold(), LinkedCode::InternalFailure,
            "loadLinked.hold", "Scene保持を開始できません");
        static_assert(std::is_nothrow_move_constructible_v<LoadLinkedRuntime>);
        loadLinked_.emplace(std::move(candidate));
        ownsHold_ = true;
        expectedInstance_ = start.request.sourceInstanceId;
        reservation_ = kInvalidSceneInstanceId;
        receipt_.reset();
        diagnostic_.clear();
        return true;
    } catch (const LinkedProgressFailure& error) {
        diagnostics.push_back({error.code, error.field, error.detail});
    } catch (const std::bad_alloc& error) {
        diagnostics.push_back({LinkedCode::ResourceFailure, "loadLinked.begin", error.what()});
    } catch (const std::exception& error) {
        diagnostics.push_back({LinkedCode::InternalFailure, "loadLinked.begin", error.what()});
    }
    return false;
}

std::optional<SceneTransitionController::LoadLinkedView> SceneTransitionController::GetLoadLinkedView() const noexcept {
    if (!loadLinked_) return std::nullopt;
    static_assert(std::is_nothrow_copy_constructible_v<LoadLinkedView>);
    const auto& state = *loadLinked_;
    return LoadLinkedView{state.identity, state.frame.stage, state.generated,
        state.frame.stage == LoadLinkedStage::AwaitCapture ? nullptr : state.frame.sample};
}
bool SceneTransitionController::NotifyLoadLinkedPresented(const LoadLinkedPresented& presented) noexcept {
    static_assert(std::is_nothrow_assignable_v<std::optional<LoadLinkedPresented>&, const LoadLinkedPresented&>);
    if (!loadLinked_) return false;
    auto& state = *loadLinked_;
    const auto expected = state.destination != kInvalidSceneInstanceId ? state.destination : state.expectedInstance;
    if (state.failed || state.finished || state.frame.suspended || state.receipt ||
        !SameLinkedIdentity(state.identity, presented.identity) || presented.drawnInstance != expected ||
        presented.recordingSerial == 0 || presented.recordingSerial <= state.lastRecordingSerial) return false;
    if (state.frame.stage == LoadLinkedStage::AwaitCapture) {
        if (presented.kind != LoadLinkedPresentationKind::Capture || presented.phase != LinkedPhase::Cutting ||
            presented.completedCuts != 0 || presented.prefixCount != 0 || presented.sampleRevision != 0) return false;
    } else {
        const auto& sample = *state.frame.sample;
        const auto kind = state.frame.stage == LoadLinkedStage::AwaitDestination
            ? LoadLinkedPresentationKind::Destination : LoadLinkedPresentationKind::Sample;
        if ((kind == LoadLinkedPresentationKind::Destination && state.destination == kInvalidSceneInstanceId) ||
            presented.kind != kind || presented.phase != sample.phase ||
            presented.completedCuts != sample.completedCuts || presented.prefixCount != sample.prefixCount ||
            presented.sampleRevision != sample.sampleRevision) return false;
    }
    state.receipt = presented;
    state.lastRecordingSerial = presented.recordingSerial;
    return true;
}
void SceneTransitionController::DiscardLoadLinkedPresentation() noexcept {
    if (!loadLinked_) return;
    // lastRecordingSerialは保持し、受領済み/破棄済みの同じ通知を再受付しない。
    loadLinked_->receipt.reset();
    loadLinked_->frame.discardDelta = true;
}
void SceneTransitionController::LatchLoadLinkedFailure(LinkedCode code, const char* field) noexcept {
    if (!loadLinked_) return;
    loadLinked_->failed = true;
    loadLinked_->failureCode = code;
    loadLinked_->failureField = field;
    loadLinked_->receipt.reset();
}
bool SceneTransitionController::UpdateLoadLinkedBeforeScene(double delta, bool suspended,
    const LoadLinkedLoadSnapshot& snapshot, LoadLinkedDiagnostics& diagnostics) {
    diagnostics.clear();
    try {
        LinkedCheck(loadLinked_.has_value(), LinkedCode::InvalidType, "loadLinked.update", "V2遷移が開始されていません");
        auto& state = *loadLinked_;
        LinkedCheck(!state.failed, state.failureCode, state.failureField, "遷移は失敗済みです。safe UpdateでCancelが必要です");
        if (!SameLinkedIdentity(snapshot.identity, state.identity)) return true;
        LinkedCheck(KnownLinkedLoadState(snapshot.state), LinkedCode::InternalFailure,
            "loadLinked.loadState", "未知の実ロード状態です");
        LinkedCheck(snapshot.statusRevision != 0 || snapshot.state == LinkedLoadState::NotStarted,
            LinkedCode::InternalFailure, "loadLinked.statusRevision", "未開始以外には観測版が必要です");
        LinkedCheck(snapshot.statusRevision >= state.statusRevision &&
            (snapshot.statusRevision != state.statusRevision || snapshot.state == state.loadState) &&
            static_cast<int>(snapshot.state) >= static_cast<int>(state.loadState),
            LinkedCode::InternalFailure, "loadLinked.statusRevision", "同版の状態矛盾または実ロード状態の後退です");
        if (snapshot.state == LinkedLoadState::Failed || snapshot.state == LinkedLoadState::Cancelled) {
            const auto code = snapshot.state == LinkedLoadState::Cancelled ? LinkedCode::Cancelled : LinkedCode::IoFailure;
            LatchLoadLinkedFailure(code, "loadLinked.loadState");
            diagnostics.push_back({code, "loadLinked.loadState",
                snapshot.diagnostic.empty() ? "実ロードが失敗または取り消されました" : snapshot.diagnostic});
            return false;
        }
        if (suspended) {
            DiscardLoadLinkedPresentation();
            state.frame.suspended = true;
            state.loadState = snapshot.state;
            state.statusRevision = snapshot.statusRevision;
            return true;
        }
        if (state.frame.suspended) {
            DiscardLoadLinkedPresentation();
            state.frame.suspended = false;
        }
        LinkedCheck(std::isfinite(delta) && delta >= 0, LinkedCode::OutOfRange,
            "loadLinked.delta", "deltaは有限/非負が必要です");
        if (state.finished) return true;
        const bool committed = state.frame.stage == LinkedStage::AwaitCommit && state.commitTaken &&
            state.destinationState == LoadLinkedDestinationState::Committed;
        if (!state.receipt && !committed) {
            state.loadState = snapshot.state;
            state.statusRevision = snapshot.statusRevision;
            return true;
        }
        // receiptは同じsampleの確認に一回だけ使う。失敗後に再消費しない。
        const auto presented = state.receipt;
        state.receipt.reset();
        if (state.frame.stage == LinkedStage::FinishPresented) {
            static_assert(std::is_nothrow_assignable_v<std::optional<LoadLinkedFinished>&, LoadLinkedFinished>);
            LinkedCheck(presented && state.binding && state.destination != kInvalidSceneInstanceId &&
                state.frame.sample->phase == LinkedPhase::Collapse &&
                state.frame.sample->evaluatedLocalTime == state.generated->Settings().collapse.duration,
                LinkedCode::InternalFailure, "loadLinked.finished", "終端の表示証拠が不正です");
            state.finished = LoadLinkedFinished{*state.binding, state.destination, state.frame.input.frozenCuts,
                state.frame.sample->sampleRevision, presented->recordingSerial};
            return true;
        }
        auto candidate = state.frame;
        const auto& settings = state.generated->Settings();
        const bool ready = snapshot.state == LinkedLoadState::Ready;
        const bool capture = candidate.stage == LinkedStage::AwaitCapture;
        if (capture) {
            candidate.stage = LinkedStage::Cutting;
            candidate.discardDelta = false;
        } else {
            LinkedCheck(candidate.input.sampleRevision != (std::numeric_limits<std::uint64_t>::max)(),
                LinkedCode::ResourceFailure, "loadLinked.sampleRevision", "sampleRevisionを更新できません");
            const double elapsed = candidate.discardDelta || committed ? 0 : delta;
            candidate.discardDelta = false;
            if (committed) {
                candidate.stage = LinkedStage::Collapse;
                candidate.input.phase = LinkedPhase::Collapse;
                candidate.input.localTime = 0;
                candidate.input.cutIndex = 0;
                candidate.input.completedCutAges = {};
                candidate.input.waitAge = 0;
                candidate.input.waitStartGlows = {};
            } else if (candidate.stage != LinkedStage::Collapse) {
                for (std::size_t i = 0; i < candidate.input.completedCuts; ++i)
                    candidate.input.completedCutAges[i] += elapsed;
                candidate.input.textAge += elapsed;
            }
            const auto enterWaiting = [&](LinkedStage next) {
                // Handoff §6どおり、直前に表示確認した同Nのglowを待機の起点にする。
                candidate.input.waitAge = 0;
                candidate.input.waitStartGlows = {};
                for (std::size_t i = 0; i < candidate.input.completedCuts; ++i)
                    candidate.input.waitStartGlows[i] = state.frame.sample->cuts[i].glow;
                candidate.stage = next;
                candidate.input.phase = next == LinkedStage::Frozen ? LinkedPhase::Frozen : LinkedPhase::Waiting;
                candidate.input.cutIndex = 0;
                candidate.input.localTime = 0;
                candidate.input.frozenCuts = next == LinkedStage::Frozen ? candidate.input.completedCuts : 0;
            };
            switch (candidate.stage) {
            case LinkedStage::Cutting:
                if (state.frame.sample->evaluatedLocalTime >= settings.cutSweepDuration) {
                    candidate.input.completedCutAges[candidate.input.completedCuts] = 0;
                    ++candidate.input.completedCuts;
                    candidate.input.localTime = 0;
                    candidate.input.cutIndex = 0;
                    candidate.intervalAge = 0;
                    if (ready && candidate.input.completedCuts >= settings.minimumCuts) enterWaiting(LinkedStage::Frozen);
                    else if (candidate.input.completedCuts == settings.maximumCuts) enterWaiting(LinkedStage::Waiting);
                    else {
                        candidate.stage = LinkedStage::Interval;
                        candidate.input.phase = LinkedPhase::Interval;
                    }
                } else {
                    candidate.input.localTime = (std::min)(static_cast<double>(settings.cutSweepDuration),
                        candidate.input.localTime + elapsed);
                }
                break;
            case LinkedStage::Interval:
                candidate.intervalAge += elapsed;
                LinkedCheck(std::isfinite(candidate.intervalAge), LinkedCode::OutOfRange,
                    "loadLinked.intervalAge", "interval時計が非有限です");
                if (ready && candidate.input.completedCuts >= settings.minimumCuts) enterWaiting(LinkedStage::Frozen);
                else if (candidate.intervalAge >= settings.cutInterval) {
                    candidate.stage = LinkedStage::Cutting;
                    candidate.input.phase = LinkedPhase::Cutting;
                    candidate.input.cutIndex = candidate.input.completedCuts;
                    candidate.input.localTime = 0;
                }
                break;
            case LinkedStage::Waiting:
                candidate.input.waitAge += elapsed;
                if (ready) enterWaiting(LinkedStage::Frozen);
                break;
            case LinkedStage::Frozen:
                candidate.input.waitAge += elapsed;
                break;
            case LinkedStage::AwaitDestination:
                candidate.input.waitAge += elapsed;
                LinkedCheck(presented && presented->kind == LoadLinkedPresentationKind::Destination && state.binding,
                    LinkedCode::InternalFailure, "loadLinked.destination", "次画面の表示証拠がありません");
                candidate.stage = LinkedStage::AwaitCommit;
                break;
            case LinkedStage::AwaitCommit:
                candidate.input.waitAge += elapsed;
                break;
            case LinkedStage::Collapse:
                if (!committed) {
                    candidate.input.localTime = (std::min)(static_cast<double>(settings.collapse.duration),
                        candidate.input.localTime + elapsed);
                    if (candidate.input.localTime >= settings.collapse.duration) candidate.stage = LinkedStage::FinishPresented;
                }
                break;
            default:
                throw LinkedProgressFailure{LinkedCode::InternalFailure, "loadLinked.stage", "未知の進行stageです"};
            }
            ++candidate.input.sampleRevision;
            LoadLinkedFrameSample sample;
            if (!TryEvaluateLoadLinked(state.generated, candidate.input, sample, diagnostics)) {
                LatchLoadLinkedFailure(diagnostics.empty() ? LinkedCode::InternalFailure : diagnostics.front().code,
                    "loadLinked.evaluation");
                return false;
            }
            candidate.sample = std::make_shared<const LoadLinkedFrameSample>(std::move(sample));
        }
        const bool froze = candidate.stage == LinkedStage::Frozen && state.frame.stage != LinkedStage::Frozen;
        const bool requestCommit = candidate.stage == LinkedStage::AwaitCommit && state.frame.stage == LinkedStage::AwaitDestination;
        static_assert(std::is_nothrow_move_assignable_v<LoadLinkedProgress>);
        static_assert(std::is_nothrow_assignable_v<std::optional<LoadLinkedFrozen>&, LoadLinkedFrozen>);
        static_assert(std::is_nothrow_assignable_v<std::optional<LoadLinkedCommitRequest>&, LoadLinkedCommitRequest>);
        state.frame = std::move(candidate);
        state.loadState = snapshot.state;
        state.statusRevision = snapshot.statusRevision;
        if (capture) state.prepareAvailable = true;
        if (froze) state.frozen = LoadLinkedFrozen{state.identity, state.generated,
            state.frame.input.frozenCuts, state.frame.sample->sampleRevision};
        if (requestCommit) state.commit = LoadLinkedCommitRequest{*state.binding};
        // 取得が同Updateより遅れても、出口のrevisionは現在のFrozen sampleへ一致させる。
        if (state.frozen) state.frozen->sampleRevision = state.frame.sample->sampleRevision;
        return true;
    } catch (const LinkedProgressFailure& error) {
        LatchLoadLinkedFailure(error.code, error.field);
        diagnostics.push_back({error.code, error.field, error.detail});
    } catch (const std::bad_alloc& error) {
        LatchLoadLinkedFailure(LinkedCode::ResourceFailure, "loadLinked.update");
        diagnostics.push_back({LinkedCode::ResourceFailure, "loadLinked.update", error.what()});
    } catch (const std::exception& error) {
        LatchLoadLinkedFailure(LinkedCode::InternalFailure, "loadLinked.update");
        diagnostics.push_back({LinkedCode::InternalFailure, "loadLinked.update", error.what()});
    } catch (...) {
        LatchLoadLinkedFailure(LinkedCode::InternalFailure, "loadLinked.update");
        throw;
    }
    return false;
}
bool SceneTransitionController::TryTakeLoadLinkedPrepareRequest(LoadLinkedPrepareRequest& out) noexcept {
    if (!loadLinked_ || loadLinked_->failed || !loadLinked_->prepareAvailable) return false;
    static_assert(std::is_nothrow_move_assignable_v<LoadLinkedPrepareRequest>);
    out = std::move(loadLinked_->prepare);
    loadLinked_->prepareAvailable = false;
    return true;
}
bool SceneTransitionController::TryTakeLoadLinkedFrozen(LoadLinkedFrozen& out) noexcept {
    if (!loadLinked_ || loadLinked_->failed || !loadLinked_->frozen) return false;
    static_assert(std::is_nothrow_move_assignable_v<LoadLinkedFrozen>);
    out = std::move(*loadLinked_->frozen);
    loadLinked_->frozen.reset();
    loadLinked_->frozenTaken = true;
    return true;
}

bool SceneTransitionController::TryBindLoadLinkedDestination(const LoadLinkedSceneBinding& binding,
    LoadLinkedDiagnostics& diagnostics) {
    diagnostics.clear();
    try {
        LinkedCheck(loadLinked_ && !loadLinked_->failed, LinkedCode::InvalidType, "loadLinked.binding", "有効なV2要求がありません");
        auto& state = *loadLinked_;
        LinkedCheck(SameLinkedIdentity(binding.identity, state.identity) && binding.tokenId != 0 && binding.tokenGeneration != 0,
            LinkedCode::ExternalChange, "loadLinked.binding", "要求と実Scene tokenが一致しません");
        if (state.binding) {
            LinkedCheck(SameLinkedBinding(*state.binding, binding), LinkedCode::ExternalChange,
                "loadLinked.binding", "固定済みtokenを差し替えられません");
            return true;
        }
        LinkedCheck(state.frame.stage == LinkedStage::Frozen && state.frozenTaken,
            LinkedCode::InvalidType, "loadLinked.binding", "Frozen出口の取得が必要です");
        state.binding = binding;
        state.frame.stage = LinkedStage::AwaitDestination;
        state.receipt.reset();
        return true;
    } catch (const LinkedProgressFailure& error) {
        diagnostics.push_back({error.code, error.field, error.detail});
    } catch (const std::bad_alloc& error) {
        diagnostics.push_back({LinkedCode::ResourceFailure, "loadLinked.binding", error.what()});
    } catch (const std::exception& error) {
        diagnostics.push_back({LinkedCode::InternalFailure, "loadLinked.binding", error.what()});
    }
    return false;
}
bool SceneTransitionController::UpdateLoadLinkedAfterScene(const LoadLinkedDestinationSnapshot& snapshot,
    LoadLinkedDiagnostics& diagnostics) {
    diagnostics.clear();
    try {
        LinkedCheck(loadLinked_ && loadLinked_->binding, LinkedCode::InvalidType,
            "loadLinked.destination", "実Scene tokenが結び付いていません");
        auto& state = *loadLinked_;
        LinkedCheck(!state.failed, state.failureCode, state.failureField, "失敗済みのV2要求です");
        if (!SameLinkedBinding(*state.binding, snapshot.binding)) return true;
        switch (snapshot.state) {
        case LoadLinkedDestinationState::Pending:
        case LoadLinkedDestinationState::Presentable:
        case LoadLinkedDestinationState::Committed:
        case LoadLinkedDestinationState::Failed:
        case LoadLinkedDestinationState::Cancelled:
            break;
        default: throw LinkedProgressFailure{LinkedCode::InternalFailure, "loadLinked.destinationState", "未知の次画面状態です"};
        }
        LinkedCheck((snapshot.statusRevision != 0 || snapshot.state == LoadLinkedDestinationState::Pending) &&
            snapshot.statusRevision >= state.destinationRevision &&
            (snapshot.statusRevision != state.destinationRevision ||
                (snapshot.state == state.destinationState && snapshot.destination == state.observedDestination)) &&
            static_cast<int>(snapshot.state) >= static_cast<int>(state.destinationState),
            LinkedCode::InternalFailure, "loadLinked.destinationRevision", "次画面の観測版または状態が後退しています");
        if (snapshot.state == LoadLinkedDestinationState::Failed || snapshot.state == LoadLinkedDestinationState::Cancelled) {
            const auto code = snapshot.state == LoadLinkedDestinationState::Cancelled ? LinkedCode::Cancelled : LinkedCode::IoFailure;
            LatchLoadLinkedFailure(code, "loadLinked.destination");
            diagnostics.push_back({code, "loadLinked.destination",
                snapshot.diagnostic.empty() ? "次画面の表示またはCommitが失敗しました" : snapshot.diagnostic});
            return false;
        }
        if (snapshot.state != LoadLinkedDestinationState::Pending) {
            LinkedCheck(snapshot.destination != kInvalidSceneInstanceId && snapshot.destination == state.binding->tokenId &&
                (state.destination == kInvalidSceneInstanceId || snapshot.destination == state.destination),
                LinkedCode::InternalFailure, "loadLinked.destination", "実destinationとtokenが一致しません");
        }
        if (snapshot.state == LoadLinkedDestinationState::Committed)
            LinkedCheck(state.commitTaken && (state.frame.stage == LinkedStage::AwaitCommit ||
                state.frame.stage == LinkedStage::Collapse || state.frame.stage == LinkedStage::FinishPresented),
                LinkedCode::InternalFailure, "loadLinked.commit", "一回Commit要求より前のCommittedです");
        state.destinationState = snapshot.state;
        state.destinationRevision = snapshot.statusRevision;
        state.observedDestination = snapshot.destination;
        if (snapshot.state != LoadLinkedDestinationState::Pending) state.destination = snapshot.destination;
        // Committedは値で保持し、次BeforeSceneだけがCollapse/sampleを公開する。
        return true;
    } catch (const LinkedProgressFailure& error) {
        LatchLoadLinkedFailure(error.code, error.field);
        diagnostics.push_back({error.code, error.field, error.detail});
    } catch (const std::bad_alloc& error) {
        LatchLoadLinkedFailure(LinkedCode::ResourceFailure, "loadLinked.destination");
        diagnostics.push_back({LinkedCode::ResourceFailure, "loadLinked.destination", error.what()});
    } catch (const std::exception& error) {
        LatchLoadLinkedFailure(LinkedCode::InternalFailure, "loadLinked.destination");
        diagnostics.push_back({LinkedCode::InternalFailure, "loadLinked.destination", error.what()});
    } catch (...) {
        LatchLoadLinkedFailure(LinkedCode::InternalFailure, "loadLinked.destination");
        throw;
    }
    return false;
}
bool SceneTransitionController::TryTakeLoadLinkedCommitRequest(LoadLinkedCommitRequest& out) noexcept {
    if (!loadLinked_ || loadLinked_->failed || !loadLinked_->commit) return false;
    static_assert(std::is_nothrow_move_assignable_v<LoadLinkedCommitRequest>);
    out = std::move(*loadLinked_->commit);
    loadLinked_->commit.reset();
    loadLinked_->commitTaken = true;
    return true;
}
bool SceneTransitionController::TryTakeLoadLinkedFinished(LoadLinkedFinished& out) noexcept {
    if (!loadLinked_ || loadLinked_->failed || !loadLinked_->finished || loadLinked_->finishedTaken) return false;
    static_assert(std::is_nothrow_copy_assignable_v<LoadLinkedFinished>);
    out = *loadLinked_->finished; // 成功Completeとの完全一致に使うscalar証拠を内部にも残す。
    loadLinked_->finishedTaken = true;
    return true;
}
bool SceneTransitionController::CompleteLoadLinked(const LoadLinkedFinished& finished, SceneManager& manager,
    LoadLinkedDiagnostics& diagnostics) {
    diagnostics.clear();
    try {
        LinkedCheck(loadLinked_ && !loadLinked_->failed && loadLinked_->finished && loadLinked_->finishedTaken,
            LinkedCode::InvalidType, "loadLinked.complete", "取得済みの成功終了証拠がありません");
        const auto& expected = *loadLinked_->finished;
        LinkedCheck(SameLinkedBinding(finished.binding, expected.binding) &&
            finished.destination == expected.destination && finished.count == expected.count &&
            finished.finalSampleRevision == expected.finalSampleRevision &&
            finished.finalRecordingSerial == expected.finalRecordingSerial &&
            manager.GetActiveSceneInstanceId() == expected.destination && ownsHold_,
            LinkedCode::ExternalChange, "loadLinked.complete", "終了証拠またはActive destinationが一致しません");
        // Gameが全GPU利用完了とRendererの安全解放を確認済み。Sceneの既存解放口はscalar更新だけ。
        manager.EndTransitionHold();
        manager.FinishPresentedTransitionRequest();
        ownsHold_ = false;
        loadLinked_.reset();
        expectedInstance_ = kInvalidSceneInstanceId;
        reservation_ = kInvalidSceneInstanceId;
        diagnostic_.clear();
        return true;
    } catch (const LinkedProgressFailure& error) {
        diagnostics.push_back({error.code, error.field, error.detail});
    } catch (const std::bad_alloc& error) {
        diagnostics.push_back({LinkedCode::ResourceFailure, "loadLinked.complete", error.what()});
    } catch (const std::exception& error) {
        diagnostics.push_back({LinkedCode::InternalFailure, "loadLinked.complete", error.what()});
    }
    return false;
}
