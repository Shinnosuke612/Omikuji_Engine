// 役割: 演出付きScene遷移の時計・snapshot・描画確認・自身の音声を所有する。
#pragma once
#include "../../engine/transition/SceneTransitionAsset.h"
#include "../../engine/transition/LoadLinkedEvaluation.h"
#include "../../engine/scene/SceneTransitionRequest.h"
#include "../../engine/audio/Audio.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class SceneManager;
class SceneCatalog;

// main thread専用。Scene/Document/GPU pointerは保存しない。
// Audio/Managerは各呼出し中の借用。破棄前にGameがCancelでVoice/保持を解放する。
class SceneTransitionController {
public:
    enum class Stage { Idle, AwaitCapture, Cutting, AwaitActivation, AwaitDestinationFrame, Collapse, FinishPresented };
    enum class LoadLinkedStage { AwaitCapture, Cutting, Interval, Waiting, Frozen,
        AwaitDestination, AwaitCommit, Collapse, FinishPresented };
    enum class LoadLinkedLoadState { NotStarted, Preparing, Ready, Failed, Cancelled };
    struct LoadLinkedIdentity {
        std::uint64_t ownerId = 0, generation = 0; // Game発行の非0・非再利用値。
        std::uint64_t requestId = 0, settingsRevision = 0; // generatedと一致。revisionは0も有効。
    };
    struct LoadLinkedStart {
        PresentedTransitionRequest request;
        std::shared_ptr<const SceneTransitionAsset::LoadLinkedGenerated> generated;
        std::uint64_t ownerId = 0, generation = 0;
    };
    struct LoadLinkedLoadSnapshot {
        LoadLinkedIdentity identity;
        LoadLinkedLoadState state = LoadLinkedLoadState::NotStarted;
        std::uint64_t statusRevision = 0; // 0はNotStartedだけ。同版は同じ実Job状態。
        std::string diagnostic;
    };
    enum class LoadLinkedPresentationKind { Capture, Sample, Destination };
    struct LoadLinkedPresented {
        LoadLinkedIdentity identity;
        LoadLinkedPresentationKind kind = LoadLinkedPresentationKind::Sample;
        SceneInstanceId drawnInstance = kInvalidSceneInstanceId;
        std::uint64_t recordingSerial = 0;
        SceneTransitionAsset::LoadLinkedEvaluationPhase phase = SceneTransitionAsset::LoadLinkedEvaluationPhase::Cutting;
        std::size_t completedCuts = 0, prefixCount = 0;
        std::uint64_t sampleRevision = 0;
    };
    struct LoadLinkedView {
        LoadLinkedIdentity identity;
        LoadLinkedStage stage;
        std::shared_ptr<const SceneTransitionAsset::LoadLinkedGenerated> generated;
        std::shared_ptr<const SceneTransitionAsset::LoadLinkedFrameSample> sample;
    };
    struct LoadLinkedPrepareRequest {
        LoadLinkedIdentity identity;
        PresentedTransitionRequest request;
    };
    struct LoadLinkedFrozen {
        LoadLinkedIdentity identity;
        std::shared_ptr<const SceneTransitionAsset::LoadLinkedGenerated> generated;
        std::size_t count = 0;
        std::uint64_t sampleRevision = 0;
    };
    struct LoadLinkedSceneBinding {
        LoadLinkedIdentity identity;
        std::uint64_t tokenId = 0, tokenGeneration = 0;
    };
    enum class LoadLinkedDestinationState { Pending, Presentable, Committed, Failed, Cancelled };
    struct LoadLinkedDestinationSnapshot {
        LoadLinkedSceneBinding binding;
        LoadLinkedDestinationState state = LoadLinkedDestinationState::Pending;
        SceneInstanceId destination = kInvalidSceneInstanceId;
        std::uint64_t statusRevision = 0;
        std::string diagnostic;
    };
    struct LoadLinkedCommitRequest { LoadLinkedSceneBinding binding; };
    struct LoadLinkedFinished {
        LoadLinkedSceneBinding binding;
        SceneInstanceId destination = kInvalidSceneInstanceId;
        std::size_t count = 0;
        std::uint64_t finalSampleRevision = 0, finalRecordingSerial = 0;
    };
    // safe Update専用。Gameがdestination登録/GPU準備を保証し、生成物だけを渡す。
    // 開始失敗/例外で旧state/holdは不変。owner/generationの非再利用はGameが保証する。
    bool TryBeginLoadLinked(const LoadLinkedStart&, SceneManager&, SceneTransitionAsset::LoadLinkedDiagnostics&);
    // 値コピーとshared所有。AwaitCaptureはsample=nullでCaptureだけを表示する。
    std::optional<LoadLinkedView> GetLoadLinkedView() const noexcept;
    // Gameが最終出力採用/可視/全GPU利用完了を確認済みのCPU射影だけを受付する。
    // 同identity/instanceと現在sampleのタグを照合し、一件だけ次Updateに保持する。
    bool NotifyLoadLinkedPresented(const LoadLinkedPresented&) noexcept;
    void DiscardLoadLinkedPresentation() noexcept;
    // false/例外後はfail latch、旧sample/N/時計を保持しGameのCancelを待つ。
    // 実Job値だけを受ける。表示時計はreceiptで進め、実Committed後はCollapse/最終表示まで評価する。
    bool UpdateLoadLinkedBeforeScene(double delta, bool suspended, const LoadLinkedLoadSnapshot&,
        SceneTransitionAsset::LoadLinkedDiagnostics&);
    // 一回だけnoexcept move。falseならout不変。取得自体はScene準備/Commitを実行しない。
    bool TryTakeLoadLinkedPrepareRequest(LoadLinkedPrepareRequest& out) noexcept;
    bool TryTakeLoadLinkedFrozen(LoadLinkedFrozen& out) noexcept;
    // Frozen出口取得後に同identityの実tokenを一回結ぶ。異なるbindingへの差替えは拒否する。
    bool TryBindLoadLinkedDestination(const LoadLinkedSceneBinding&, SceneTransitionAsset::LoadLinkedDiagnostics&);
    // 同bindingの実Presentable/Committedを受ける。Committed後のCollapse公開は次のBeforeScene。
    bool UpdateLoadLinkedAfterScene(const LoadLinkedDestinationSnapshot&, SceneTransitionAsset::LoadLinkedDiagnostics&);
    // 一回取得でfalse時out保持。Finished取得後もComplete成功までactive/holdを維持する。
    bool TryTakeLoadLinkedCommitRequest(LoadLinkedCommitRequest& out) noexcept;
    bool TryTakeLoadLinkedFinished(LoadLinkedFinished& out) noexcept;
    // Finished取得値の完全一致とActive destinationを照合して成功解放。GPU安全解放はGameが先に保証。
    bool CompleteLoadLinked(const LoadLinkedFinished&, SceneManager&, SceneTransitionAsset::LoadLinkedDiagnostics&);
    // IdleからIO/Decodeだけを行う。失敗時はSceneや保持を変更しない。
    bool TryPrepareRequest(const PresentedTransitionRequest&, const SceneCatalog&, Audio&,
        SceneTransitionAsset::Diagnostics&);
    // 通常dispatchが同一読取から得たV1を受ける。旧入口とcue/評価を共有し再読取しない。
    bool TryPrepareDecodedRequest(const PresentedTransitionRequest&,
        std::shared_ptr<const SceneTransitionAsset::CompiledTransition>, const SceneCatalog&, Audio&,
        SceneTransitionAsset::Diagnostics&);
    // GPU準備成功後に一回。保持開始に成功した場合だけAwaitCaptureへ移る。
    bool BeginPrepared(SceneManager&, SceneTransitionAsset::Diagnostics&);
    // 前Frameの全GPU利用完了を確認した後、通常Scene更新より前に一回呼ぶ。
    void UpdateBeforeScene(SceneManager&, Audio&, float delta, bool suspended, std::uint64_t completedSerial);
    // 保持更新による予約確定後にID一致の結果だけを一回消費する。
    void UpdateAfterScene(SceneManager&, Audio&);
    // Drawはcommand記録だけ。可視Frameの最終Fence完了serialと照合して次Updateで消費する。
    void NotifyDrawRecorded(std::uint64_t serial, SceneInstanceId drawnInstance);
    void DiscardDrawReceipt();
    // safe Update/最後のGPU完了後だけ。自己予約/Voice/保持だけを解除し完了Eventは出さない。
    void Cancel(SceneManager&, Audio&, const std::string& reason = {});

    bool IsActive() const { return stage_ != Stage::Idle || loadLinked_.has_value(); }
    Stage GetStage() const { return stage_; } // V1専用。V2はGetLoadLinkedViewを使う。
    // 次のUpdate/Cancelまでの借用参照。GameはDraw内だけsampleを参照する。
    const std::shared_ptr<const SceneTransitionAsset::CompiledTransition>& GetAsset() const { return asset_; }
    const SceneTransitionAsset::FrameSample& GetSample() const { return sample_; }
    SceneInstanceId GetExpectedInstance() const { return expectedInstance_; }
    const std::string& GetDiagnostic() const { return diagnostic_; }

private:
    struct LoadLinkedProgress {
        LoadLinkedStage stage = LoadLinkedStage::AwaitCapture;
        SceneTransitionAsset::LoadLinkedPresentationInput input;
        std::shared_ptr<const SceneTransitionAsset::LoadLinkedFrameSample> sample;
        double intervalAge = 0;
        bool suspended = false, discardDelta = true;
    };
    // modeはこのoptionalの有無で分離。frame候補は固定値/shared所有だけをコピーする。
    struct LoadLinkedRuntime {
        LoadLinkedIdentity identity;
        std::shared_ptr<const SceneTransitionAsset::LoadLinkedGenerated> generated;
        SceneInstanceId expectedInstance = kInvalidSceneInstanceId;
        LoadLinkedProgress frame;
        std::optional<LoadLinkedPresented> receipt;
        std::uint64_t lastRecordingSerial = 0, statusRevision = 0;
        LoadLinkedLoadState loadState = LoadLinkedLoadState::NotStarted;
        LoadLinkedPrepareRequest prepare;
        bool prepareAvailable = false, failed = false;
        std::optional<LoadLinkedFrozen> frozen;
        bool frozenTaken = false, commitTaken = false, finishedTaken = false;
        std::optional<LoadLinkedSceneBinding> binding;
        LoadLinkedDestinationState destinationState = LoadLinkedDestinationState::Pending;
        SceneInstanceId destination = kInvalidSceneInstanceId, observedDestination = kInvalidSceneInstanceId;
        std::uint64_t destinationRevision = 0;
        std::optional<LoadLinkedCommitRequest> commit;
        std::optional<LoadLinkedFinished> finished;
        SceneTransitionAsset::LoadLinkedDiagnosticCode failureCode = SceneTransitionAsset::LoadLinkedDiagnosticCode::InternalFailure;
        const char* failureField = "loadLinked";
    };
    void LatchLoadLinkedFailure(SceneTransitionAsset::LoadLinkedDiagnosticCode, const char* field) noexcept;
    std::optional<LoadLinkedRuntime> loadLinked_;
    struct CueVoice { SceneTransitionAsset::AudioCue cue; AudioClipPtr clip; bool fired = false; };
    struct Receipt {
        std::uint64_t generation, serial;
        Stage stage;
        SceneInstanceId instance;
        float time;
    };
    bool Evaluate(SceneManager&, Audio&);
    void FireCues(Audio&, SceneTransitionAsset::Phase, double previous, double current, bool phaseStart);
    void StopVoices(Audio&);
    PresentedTransitionRequest request_;
    std::shared_ptr<const SceneTransitionAsset::CompiledTransition> asset_;
    SceneTransitionAsset::FrameSample sample_;
    std::vector<CueVoice> cues_;
    std::vector<Audio::PlaybackHandle> voices_;
    std::optional<Receipt> receipt_;
    Stage stage_ = Stage::Idle;
    SceneInstanceId reservation_ = kInvalidSceneInstanceId, expectedInstance_ = kInvalidSceneInstanceId;
    std::uint64_t generation_ = 0;
    double time_ = 0;
    bool ownsHold_ = false, suspended_ = false, discardDelta_ = true;
    std::string diagnostic_;
};
