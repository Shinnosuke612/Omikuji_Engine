// 役割: StatSetの保存値から実行時Statを生成し、値と範囲の変更を一元管理する。
#pragma once

#include <cstdint>
#include <string>
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
	void Clear();

private:
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
