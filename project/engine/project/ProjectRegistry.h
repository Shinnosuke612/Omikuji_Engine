// 役割: PC localのProject Launcher登録情報を管理する。
#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

struct ProjectRegistryEntry {
	std::filesystem::path descriptorPath;
	bool pinned = false;
	std::string lastOpenedUtc;
};

struct ProjectDependencyOverride {
	std::string projectId;
	std::string dependencyId;
	std::filesystem::path checkoutPath;
};

enum class ProjectCreationOperationState {
	InProgress,
	Failed,
	RegistrationIncomplete,
	Complete
};

struct ProjectCreationOperation {
	std::string operationId;
	std::string projectId;
	std::string displayName;
	std::string startSceneId;
	std::filesystem::path destinationRoot;
	std::filesystem::path finalProjectRoot;
	std::filesystem::path templateSourceRoot;
	std::filesystem::path generatorPath;
	std::filesystem::path logPath;
	uint32_t processId = 0;
	uint64_t processStartTimeFileTime = 0;
	int32_t exitCode = -1;
	ProjectCreationOperationState state = ProjectCreationOperationState::InProgress;
	// 空は旧journal。新規作成だけ出力layoutを明示して保存する。
	std::string outputLayout;
};

enum class SolutionGenerationOperationFileKind {
	Artifact,
	Descriptor,
	Manifest
};

struct SolutionGenerationOperationFile {
	SolutionGenerationOperationFileKind kind = SolutionGenerationOperationFileKind::Artifact;
	std::filesystem::path relativePath;
	std::string previousContentHash;
	std::string nextContentHash;
	bool previousExists = false;
	bool nextExists = true;
};

enum class SolutionGenerationOperationState {
	Staged,
	CommitInProgress,
	RollbackInProgress,
	RolledBack,
	Complete,
	RecoveryRequired
};

struct SolutionGenerationOperation {
	std::string operationId;
	std::string projectId;
	std::filesystem::path projectRoot;
	std::filesystem::path stagingRoot;
	std::filesystem::path rollbackRoot;
	std::vector<SolutionGenerationOperationFile> files;
	SolutionGenerationOperationState state = SolutionGenerationOperationState::Staged;
};

class ProjectRegistry {
public:
	static constexpr int kSchemaVersion = 3;

	bool Load(std::string& errorMessage);
	bool Load(const std::filesystem::path& registryPath, std::string& errorMessage);
	bool Save(std::string& errorMessage) const;
	bool Validate(std::string& errorMessage) const;

	bool RegisterProject(const ProjectRegistryEntry& entry, std::string& errorMessage);
	bool RemoveProject(const std::filesystem::path& descriptorPath);
	bool AddWorkspaceRoot(const std::filesystem::path& root, std::string& errorMessage);
	bool RemoveWorkspaceRoot(const std::filesystem::path& root);
	bool SetDependencyOverride(const ProjectDependencyOverride& value, std::string& errorMessage);
	bool RemoveDependencyOverride(const std::string& projectId, const std::string& dependencyId);
	bool UpsertCreationOperation(const ProjectCreationOperation& value, std::string& errorMessage);
	const ProjectCreationOperation* FindCreationOperation(const std::string& operationId) const;
	// Local Registryへ生成transactionの再起動復旧情報を保存する。Project内の生成物は変更しない。
	bool UpsertSolutionGenerationOperation(const SolutionGenerationOperation& value, std::string& errorMessage);
	const SolutionGenerationOperation* FindSolutionGenerationOperation(const std::string& operationId) const;

	const std::filesystem::path& GetRegistryPath() const { return registryPath_; }
	const std::filesystem::path& GetTemplateSourceRoot() const { return templateSourceRoot_; }
	const std::string& GetPreferredVisualStudioInstanceId() const { return preferredVisualStudioInstanceId_; }
	const std::vector<ProjectRegistryEntry>& GetProjects() const { return projects_; }
	const std::vector<std::filesystem::path>& GetWorkspaceRoots() const { return workspaceRoots_; }
	const std::vector<ProjectDependencyOverride>& GetDependencyOverrides() const { return dependencyOverrides_; }
	const std::vector<ProjectCreationOperation>& GetCreationOperations() const { return creationOperations_; }
	// Launcherはこのjournalをread-onlyで参照し、復旧可能な操作だけを提示する。
	const std::vector<SolutionGenerationOperation>& GetSolutionGenerationOperations() const { return solutionGenerationOperations_; }

	void SetTemplateSourceRoot(const std::filesystem::path& value) { templateSourceRoot_ = value; }
	void SetPreferredVisualStudioInstanceId(const std::string& value) { preferredVisualStudioInstanceId_ = value; }

	static bool TryGetDefaultRegistryPath(std::filesystem::path& path, std::string& errorMessage);

private:
	static bool AreSamePath(const std::filesystem::path& left, const std::filesystem::path& right);
	static std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path& value);
	static const char* CreationOperationStateToString(ProjectCreationOperationState value);
	static bool TryParseCreationOperationState(const std::string& value, ProjectCreationOperationState& state);
	static const char* SolutionGenerationOperationStateToString(SolutionGenerationOperationState value);
	static bool TryParseSolutionGenerationOperationState(const std::string& value, SolutionGenerationOperationState& state);
	static const char* SolutionGenerationOperationFileKindToString(SolutionGenerationOperationFileKind value);
	static bool TryParseSolutionGenerationOperationFileKind(const std::string& value, SolutionGenerationOperationFileKind& kind);

	std::filesystem::path registryPath_;
	std::filesystem::path templateSourceRoot_;
	std::string preferredVisualStudioInstanceId_;
	std::vector<ProjectRegistryEntry> projects_;
	std::vector<std::filesystem::path> workspaceRoots_;
	std::vector<ProjectDependencyOverride> dependencyOverrides_;
	std::vector<ProjectCreationOperation> creationOperations_;
	std::vector<SolutionGenerationOperation> solutionGenerationOperations_;
};
