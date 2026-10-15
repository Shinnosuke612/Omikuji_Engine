// 役割: 演出付きScene遷移の時計・snapshot・描画確認・自身の音声を所有する。
#pragma once
#include "../../engine/transition/SceneTransitionAsset.h"
#include "../../engine/scene/SceneTransitionRequest.h"
#include "../../engine/audio/Audio.h"
#include <optional>

class SceneManager;
class SceneCatalog;

// main thread専用。Scene/Document/GPU pointerは保存しない。
// Audio/Managerは各呼出し中の借用。破棄前にGameがCancelでVoice/保持を解放する。
class SceneTransitionController {
public:
    enum class Stage { Idle, AwaitCapture, Cutting, AwaitActivation, AwaitDestinationFrame, Collapse, FinishPresented };
    // IdleからIO/Decodeだけを行う。失敗時はSceneや保持を変更しない。
    bool TryPrepareRequest(const PresentedTransitionRequest&, const SceneCatalog&, Audio&,
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

    bool IsActive() const { return stage_ != Stage::Idle; }
    Stage GetStage() const { return stage_; }
    // 次のUpdate/Cancelまでの借用参照。GameはDraw内だけsampleを参照する。
    const std::shared_ptr<const SceneTransitionAsset::CompiledTransition>& GetAsset() const { return asset_; }
    const SceneTransitionAsset::FrameSample& GetSample() const { return sample_; }
    SceneInstanceId GetExpectedInstance() const { return expectedInstance_; }
    const std::string& GetDiagnostic() const { return diagnostic_; }

private:
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
