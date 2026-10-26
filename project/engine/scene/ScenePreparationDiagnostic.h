// 役割: Scene準備の失敗を、RuntimeからEditorへUIに依存せず値で渡す。
#pragma once
#include <cstdint>
#include <string>

enum class ScenePreparationDiagnosticCode {
	None, InvalidSettings, InvalidReference, UnsupportedPrefab, CapacityExceeded,
	SpawnFailed, InstanceLost, WaveMismatch
};

struct ScenePreparationDiagnostic {
	ScenePreparationDiagnosticCode code = ScenePreparationDiagnosticCode::None;
	uint64_t documentGeneration = 0;
	uint64_t entityId = 0;
	uint64_t serial = 0;
	// 技術的な補助情報。人間向け原因/対処はEditorの既存言語設定から表示する。
	std::string detail;
};
