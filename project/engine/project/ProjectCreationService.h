// 役割: New-GameProject generatorの要求、journal、finalize、recoveryを所有する。
#pragma once

#include "ProjectDescriptor.h"
#include "ProjectRegistry.h"
#include "WindowsProcess.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ProjectCreationRequest {
	std::string projectId;
	std::string displayName;
	std::filesystem::path destinationRoot;
	std::string startSceneId = "main";
	std::filesystem::path templateSourceRoot;
};

enum class ProjectCreationServiceState {
	Idle,
	GeneratorRunning,
	RegistrationIncomplete,
	Failed,
	Complete
};

struct ProjectCreationServiceSnapshot {
	ProjectCreationServiceState state = ProjectCreationServiceState::Idle;
	std::optional<ProjectCreationOperation> operation;
	std::string statusMessage;
};

class ProjectCreationService {
public:
	explicit ProjectCreationService(ProjectRegistry& registry);

	bool Start(const ProjectCreationRequest& request, std::string& errorMessage);
	bool Update(std::string& errorMessage);
	bool Reconcile(std::string& errorMessage);
	bool RetryFinalize(const std::string& operationId, std::string& errorMessage);
	std::vector<std::filesystem::path> FindOrphanedStagingCandidates(
		const std::string& operationId,
		std::string& errorMessage
	) const;
	const ProjectCreationServiceSnapshot& GetSnapshot() const { return snapshot_; }

private:
	static bool ValidateRequest(
		const ProjectCreationRequest& request,
		ProjectCreationOperation& operation,
		std::string& errorMessage
	);
	// 初回はLegacy準備物、journal作成後は復旧入口としてroot/Assetとjournalの一致を確認する。
	bool ValidateFinalTree(
		const ProjectCreationOperation& operation,
		std::string& errorMessage
	);
	static bool ConfigureDescriptor(
		const ProjectCreationOperation& operation,
		ProjectDescriptor& descriptor,
		std::string& errorMessage,
		bool groupedLayout
	);
	static std::filesystem::path FindPowerShell();
	static uint64_t GetProcessStartTimeFileTime(uint32_t processId);
	static bool IsProcessIdentityRunning(uint32_t processId, uint64_t startTimeFileTime);
	static bool HasReparsePointInExistingPath(const std::filesystem::path& path);
	static bool AreSamePath(const std::filesystem::path& left, const std::filesystem::path& right);
	static std::string MakeOperationId();

	bool PersistOperation(ProjectCreationOperation& operation, std::string& errorMessage);
	bool Finalize(ProjectCreationOperation& operation, std::string& errorMessage);
	void SetSnapshot(ProjectCreationServiceState state, const ProjectCreationOperation& operation, const std::string& message);

	ProjectRegistry& registry_;
	WindowsProcess generatorProcess_;
	std::optional<ProjectCreationOperation> activeOperation_;
	ProjectCreationServiceSnapshot snapshot_{};
};
