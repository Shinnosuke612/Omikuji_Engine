// 役割: Runtime候補と各Systemで共有する段階準備の進捗と予算を定義する。
#pragma once
#include <cstdint>
#include <chrono>
#include <string_view>

// TEMP: 搬送待ち診断後に撤去。既存Text Step内の実観測だけをmainへ通知する。
struct ScenePreparedTextObservation {
    uint64_t entity = 0, bytes = 0;
    const char* event = "unknown";
    int64_t atUs = 0;
    std::string_view key; // callback中だけ借用。文字内容は診断へ渡さない。
};

// TEMP: 並行化の比較後に撤去する候補内の実進捗。mainだけが更新/参照する。
struct SceneRuntimePrepareMetrics {
    uint64_t entities = 0, localStarted = 0, localReady = 0, bound = 0, skippedAhead = 0;
    uint64_t waitModel = 0, waitTexture = 0, waitEnvironment = 0;
    uint64_t lastEntity = 0, lastIndex = 0;
    const char* action = "idle";
};

struct SceneRuntimePrepareBudget {
    uint32_t maxItems = 0;
    uint32_t maxResourceCreates = 0;
    uint64_t maxCopyBytes = 0;
    // 同Updateの絶対期限を下層まで共有。既存呼出しは無期限/全素材完成として互換動作する。
    std::chrono::steady_clock::time_point deadline = (std::chrono::steady_clock::time_point::max)();
    bool allowRuntimeFinalize = true;
    uint64_t assetRevision = 0;
    SceneRuntimePrepareMetrics* metrics = nullptr; // 呼出し中だけ借用、workerへ渡さない。
    // TEMP: 同期Step中だけ借用し、保存/worker転送/処理指示に使用しない。nullなら観測なし。
    void* textObservationContext = nullptr;
    void (*textObservation)(void*, const ScenePreparedTextObservation&) noexcept = nullptr;
};

// Progressは同期限内で再Pump可能。WaitingForAssetsは素材barrier前だけの待機で、Readyではない。
enum class RuntimePrepareStep { Pending, Ready, Failed, Unsupported, Progress, WaitingForAssets };
