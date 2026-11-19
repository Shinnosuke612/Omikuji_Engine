// 役割: 一プレイの対象Player・合成倍率・取得履歴を所有し、保存値から分離する。
#pragma once
#include "../../../../../engine/scene/SceneRunUpgradeRuntime.h"
#include "../../../../../engine/scene/SceneDocument.h"
#include <unordered_map>
#include <random>
#include "../../../../../engine/scene/SceneMenuRuntime.h"

class SceneStatSystem;

// RuntimeScene所有。選択tokenとMenu通知を検証し、直接付与Actionは提供しない。
class SceneRunUpgradeSystem {
public:
	// GameFlow開始pulseだけで呼ぶ。前のプレイ状態は破棄する。
	void BeginRun(uint64_t instance, uint64_t generation, uint64_t directorId, uint64_t playerId);
	// 通常更新/Menu prepassの前。定義/ownerの消失・内容交換では取得状態も破棄する。
	void Sync(const SceneDocument& document, uint64_t instance);
	// 次のSync/Reset/Clearまでの読取用参照。consumerは呼出しを越えて保持しない。
	const SceneRunUpgradeModifiers& GetModifiers() const { return modifiers_; }
	// Player/DirectorのPool復元では取得状態を捨てる。フェーズ切替では呼ばない。
	void ResetEntity(uint64_t id);
	void Clear();
	// 同じtokenは既存候補を返す。失敗時は診断を返し、抽選を繰り返して補わない。
	std::string BeginSelection(const SceneDocument&, const SceneUpgradeToken&, uint64_t panelId, const std::vector<SceneUpgradePoolEntry>&);
	bool ValidateSelection(const SceneDocument&, const SceneUpgradeToken&);
	bool BindMenuSource(const SceneMenuSource& source);
	bool Select(const SceneDocument&, const SceneMenuSource&, const SceneUpgradeToken&, uint64_t itemId);
	bool TryConfirm(const SceneDocument&, SceneStatSystem&, const SceneMenuSource&, const SceneUpgradeToken&, uint64_t itemId);
	SceneUpgradePresentation GetPresentation(uint64_t hoveredItemId) const;
	uint64_t SelectionPanel() const { return selectionPanel_; }
	bool Committed() const { return committed_; }
	const SceneUpgradeToken& SelectionToken() const { return token_; }
	// Resume成功後に表示sessionだけを破棄。倍率/履歴は次フェーズへ持ち越す。
	void EndSelection();
private:
	// TryConfirmのcommit専用。選択token検証なしに外部から呼べる入口にしない。
	// 必要な確保と倍率計算を先に行い、HP変更後には失敗し得る確保を置かない。
	bool ApplyDefinition(const SceneDocument& document, SceneStatSystem& stats, const SceneRunUpgradeDefinition& definition);
	uint64_t instance_ = 0, generation_ = 0, director_ = 0, definitionComponent_ = 0, playerComponent_ = 0;
	bool bound_ = false;
	SceneRunUpgradeModifiers modifiers_;
	std::vector<SceneRunUpgradeDefinition> definitions_;
	std::unordered_map<std::string, uint64_t> acquired_;
	bool SourceMatches(const SceneDocument&, const SceneMenuSource&) const;
	std::mt19937_64 random_{};
	SceneUpgradeToken token_{};
	SceneMenuSource menuSource_{};
	uint64_t selectionPanel_ = 0, selectionComponent_ = 0, confirmItem_ = 0, lastSelectionSerial_ = 0;
	std::array<SceneUpgradeSlot, 3> slots_{};
	std::array<SceneRunUpgradeDefinition, 3> choices_{};
	std::vector<SceneUpgradePoolEntry> pool_;
	int selected_ = -1;
	bool committed_ = false;
};
