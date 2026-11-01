// 役割: 履歴の読取専用状態と描画前通知。applicationの履歴実体は公開しない。
#pragma once
#include <cstdint>
#include <string>

enum class SceneReplayHistoryPhase { Idle, Recording, Sealed, Failed };
struct SceneReplayHistoryStatus {
    SceneReplayHistoryPhase phase = SceneReplayHistoryPhase::Idle;
    uint64_t owner = 0, sampleCount = 0, countedBytes = 0, thinningCount = 0;
    double realSeconds = 0;
    std::string diagnostic;
};
struct SceneReplayCaptureRequest {
    uint64_t frameSerial = 0;
    float gameViewEffectTime = 0;
};
