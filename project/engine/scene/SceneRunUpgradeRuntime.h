// 役割: 一プレイ内の強化倍率をconsumerへ渡す非保存の値snapshotを定義する。
#pragma once
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>

// Authoringのpool/表示参照。取得結果や入力状態は保存しない。
struct SceneUpgradePoolEntry {
	std::string id;
	float weight = 1;
	bool operator==(const SceneUpgradePoolEntry&) const = default;
};
struct SceneUpgradeSlot {
	uint64_t itemEntityId = 0, summaryTextEntityId = 0, detailRootEntityId = 0;
	uint64_t titleTextEntityId = 0, descriptionTextEntityId = 0, selectedMarkerEntityId = 0;
	bool operator==(const SceneUpgradeSlot&) const = default;
};
// Flowが発行する一回の選択待ち。内容交換/別プレイ/別フェーズへの転用を拒否する。
struct SceneUpgradeToken {
	uint64_t instance = 0, generation = 0, directorId = 0, selectionSerial = 0;
	std::string clearedPhaseId;
	bool operator==(const SceneUpgradeToken&) const = default;
};
struct SceneUpgradePresentation {
	std::unordered_map<uint64_t, std::string> texts;
	std::vector<uint64_t> hiddenRoots, disabledItems;
};

struct SceneRunUpgradeValues {
	float damage = 1, moveSpeed = 1, attackRange = 1, knockback = 1, recovery = 1;
	bool IsValid() const {
		return std::isfinite(damage) && damage >= 1 && std::isfinite(moveSpeed) && moveSpeed >= 1 &&
			std::isfinite(attackRange) && attackRange >= 1 && std::isfinite(knockback) && knockback >= 1 &&
			std::isfinite(recovery) && recovery > 0 && recovery <= 1;
	}
};

struct SceneRunUpgradeModifiers {
	uint64_t playerEntityId = 0;
	SceneRunUpgradeValues values;
	// 未登録owner/不正snapshotには従来値を返す。EntityやDocumentは保持しない。
	SceneRunUpgradeValues ForOwner(uint64_t owner) const {
		return owner && owner == playerEntityId && values.IsValid() ? values : SceneRunUpgradeValues{};
	}
};

// 攻撃時計/姿勢の再生速度は変えず、保護終端以降の秒だけを写像する。
struct SceneAttackRecoveryTiming {
	float authoredDuration = 0, protectedEnd = 0, multiplier = 1;
	float MapTime(float time) const {
		return multiplier == 1 || time <= protectedEnd ? time : protectedEnd + (time - protectedEnd) * multiplier;
	}
	float Duration() const { return MapTime(authoredDuration); }
};
