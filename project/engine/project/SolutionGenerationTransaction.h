// 役割: preview artifactをowned-file限定でcommitし、rollback/restart recoveryを所有する。
#pragma once

#include "ProjectRegistry.h"
#include "SolutionGenerationEmitter.h"

#include <filesystem>
#include <string>
#include <vector>

struct SolutionGenerationTransactionRequest {
	std::string operationId;
	std::string projectId;
	std::filesystem::path projectRoot;
	SolutionGenerationPreview preview;
	std::vector<SolutionGenerationOperationFile> files;
	// 空なら同path更新。異なるpathは固定V1→V2移行の旧manifestだけを指定する。
	std::filesystem::path previousManifestPath;
};

enum class SolutionGenerationRecoveryResult {
	NoOperation,
	Completed,
	RolledBack,
	RecoveryRequired
};

class SolutionGenerationTransaction {
public:
	// previewと既存manifestを照合し、commit前のjournalだけをLocal Registryへ保存する。
	bool Stage(const SolutionGenerationTransactionRequest& request, ProjectRegistry& registry, std::string& errorMessage) const;
	// journal済みowned artifactだけをfinal pathへ移動する。Launcherが明示採用を許可した時だけ呼び出す。
	bool Commit(const std::string& operationId, ProjectRegistry& registry, std::string& errorMessage) const;
	bool CanCommit(const std::string& operationId, const ProjectRegistry& registry, std::string& errorMessage) const;
	bool ResumeCommit(const std::string& operationId, ProjectRegistry& registry, std::string& errorMessage) const;
	bool CanResumeCommit(const std::string& operationId, const ProjectRegistry& registry, std::string& errorMessage) const;
	// rollback areaにあるjournal済みの旧artifactだけを復元する。
	bool RestorePrevious(const std::string& operationId, ProjectRegistry& registry, std::string& errorMessage) const;
	// partial commitでもold/new hashだけから旧状態へ安全に戻せるかをread-onlyで判定する。
	bool CanRestorePrevious(const std::string& operationId, const ProjectRegistry& registry, std::string& errorMessage) const;
	// 再起動時にold/new setの完全hash一致だけを確定し、混在setは復旧要求として残す。
	SolutionGenerationRecoveryResult Recover(const std::string& operationId, ProjectRegistry& registry, std::string& errorMessage) const;
};
