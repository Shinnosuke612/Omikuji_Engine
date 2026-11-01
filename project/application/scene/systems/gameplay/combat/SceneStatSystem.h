// 役割: StatSetの保存値から実行時Statを生成し、値と範囲の変更を一元管理する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

class SceneDocument;

class SceneStatSystem {
public:
	void Update(const SceneDocument& document);
	bool Modify(
		uint64_t entityId,
		const std::string& statId,
		const std::string& operation,
		float value
	);
	bool TryGet(
		uint64_t entityId,
		const std::string& statId,
		float& value,
		float* minValue = nullptr,
		float* maxValue = nullptr
	) const;
	bool IsAtMin(uint64_t entityId, const std::string& statId) const;
	// 未定義/型違いはfalse。初期化は変更通知にならず、同値Setもrevisionを増やさない。
	bool TryGetFlag(uint64_t entityId, const std::string& flagId, bool& value, uint64_t* revision = nullptr) const;
	bool SetFlag(uint64_t entityId, const std::string& flagId, bool value);
	// DeathResponseだけが登録するHP/flagの整合制約。生存値は既存Mapのまま。
	struct DeathBinding { uint64_t owner = 0, stats = 0; std::string hp, flag; };
	void SetDeathBindings(std::vector<DeathBinding> bindings);
	// 死亡確定の変更番号。通常のStat/flag更新では増やさない。
	uint64_t DeathRevision() const { return deathRevision_; }
	bool IsManagedFlag(uint64_t stats, const std::string& flag) const;
	bool IsManagedDead(uint64_t stats, const std::string& hp) const;
	// Pool baseline復元時だけ現在値と管理関係を破棄する。
	void ResetEntity(uint64_t id);
	void Clear();

private:
	std::vector<DeathBinding> deathBindings_;
	uint64_t deathRevision_ = 0;
	struct FlagRuntime { bool value = false; uint64_t revision = 0; };
	std::unordered_map<uint64_t, std::unordered_map<std::string, FlagRuntime>> flags_;
	// Entity再生成でも古い変更番号を再利用しない。Scene終了時にだけClearする。
	uint64_t nextFlagRevision_ = 0;
	struct StatRuntime {
		float value = 0.0f;
		float minValue = 0.0f;
		float maxValue = 0.0f;
	};

	std::unordered_map<
		uint64_t,
		std::unordered_map<std::string, StatRuntime>
	> runtimes_;
};
