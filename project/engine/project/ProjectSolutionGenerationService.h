// 役割: Project Manager用のSolution preview workspaceと生成復旧診断を所有する。
#pragma once

#include "ProjectRegistry.h"
#include "SolutionGenerationEmitter.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class ProjectDescriptor;

enum class ProjectSolutionPreviewState {
	Unavailable,
	Ready,
	PreviewReady,
	Failed
};

struct ProjectSolutionPreviewSnapshot {
	ProjectSolutionPreviewState state = ProjectSolutionPreviewState::Unavailable;
	std::string projectId;
	std::string operationId;
	// PreviewReadyでは必須の生成元。PC local originから復元し、別checkoutへ使い回さない。
	std::filesystem::path descriptorPath;
	std::filesystem::path stagingRoot;
	std::filesystem::path solutionPath;
	std::filesystem::path legacyArtifactDirectory;
	std::filesystem::path groupedArtifactDirectory;
	std::string inputIdentity;
	std::vector<SolutionGenerationArtifact> artifacts;
	std::string detail;
	uint32_t retiredArtifactCount = 0;
	uint32_t modifiedOwnedArtifactCount = 0;
	bool layoutMigrationRequired = false;
	bool canMigrateOutputLayout = false;
};

struct ProjectSolutionRecoverySnapshot {
	std::string operationId;
	std::string projectId;
	SolutionGenerationOperationState state = SolutionGenerationOperationState::RecoveryRequired;
	std::filesystem::path stagingRoot;
	std::filesystem::path rollbackRoot;
	uint32_t fileCount = 0;
	bool canRecheck = false;
	bool canCommitStaged = false;
	bool canResumeCommit = false;
	bool canRestorePrevious = false;
	std::string detail;
};

class ProjectSolutionGenerationService {
public:
	// Registryは呼び出し側が所有し、service lifetime中は有効でなければならない。
	explicit ProjectSolutionGenerationService(ProjectRegistry& registry);

	// descriptorとbuild specificationをread-onlyで診断する。仕様なしの旧Project／managed-sourceはUnavailableを返す。
	bool InspectProject(const std::filesystem::path& descriptorPath, ProjectSolutionPreviewSnapshot& output, std::string& errorMessage) const;
	// PC local workspaceへpreviewと所有元情報を生成する。canonical Projectのfileは変更しない。
	bool CreatePreview(const std::filesystem::path& descriptorPath, ProjectSolutionPreviewSnapshot& output, std::string& errorMessage);
	// 初回生成と再開をjournalのlayoutで確定する。旧呼出しはGrouped V1を維持する。
	bool CreateInitialGroupedGeneration(
		const ProjectDescriptor& descriptor,
		const std::string& operationId,
		const std::vector<std::filesystem::path>& retiredLegacyArtifacts,
		std::string& errorMessage,
		SolutionGenerationLayout outputLayout = SolutionGenerationLayout::GroupedV1
	);
	// Adopt／Regenerate／Migrateは生成元descriptorの一致と有効なoriginを必須とする。
	bool CanAdoptPreview(const std::filesystem::path& descriptorPath, const std::string& operationId, std::string& errorMessage) const;
	bool AdoptPreview(const std::filesystem::path& descriptorPath, const std::string& operationId, std::string& errorMessage);
	// 生成元が一致しoriginも再検証できた候補だけを返す。旧originなしPreviewは再生成が必要。
	// 返却値はpreviews_変更まで有効な借用。操作可否より現在入力との一致を優先する。
	const ProjectSolutionPreviewSnapshot* FindPreferredPreview(const std::filesystem::path& descriptorPath,
		bool& matchesCurrentInput, std::string& errorMessage) const;
	// 現行V2のowned hashが一致する場合だけ更新を許可し、手編集や未完了journalは拒否する。
	bool CanRegeneratePreview(const std::filesystem::path& descriptorPath, const std::string& operationId, std::string& errorMessage) const;
	// 指定Previewを再検証し、owned artifactとmanifestだけを既存journal／rollback経路で更新する。
	bool RegeneratePreview(const std::filesystem::path& descriptorPath, const std::string& operationId, std::string& errorMessage);
	// 旧配置からV2への明示移行。旧owned hashが一致する場合だけdescriptorとIDE集合を更新する。
	bool CanMigrateOutputLayout(const std::filesystem::path& descriptorPath, const std::string& operationId, std::string& errorMessage) const;
	bool MigrateOutputLayout(const std::filesystem::path& descriptorPath, const std::string& operationId, std::string& errorMessage);
	bool CommitStaged(const std::string& operationId, std::string& errorMessage);
	bool ResumeCommit(const std::string& operationId, std::string& errorMessage);
	// 固定preview rootとRegistry journalを再起動後に安全なread-only状態へ復元する。
	bool DiscoverPreviews(std::string& errorMessage);
	bool ReconcileRecovery(std::string& errorMessage);
	bool RecheckRecovery(const std::string& operationId, std::string& errorMessage);
	bool RestorePrevious(const std::string& operationId, std::string& errorMessage);

	const std::vector<ProjectSolutionPreviewSnapshot>& GetPreviews() const { return previews_; }
	const std::vector<ProjectSolutionRecoverySnapshot>& GetRecoverySnapshots() const { return recoveries_; }

private:
	std::filesystem::path GetPreviewRoot() const;
	void RefreshRecoverySnapshots();
	void UpsertPreview(ProjectSolutionPreviewSnapshot value);
	bool BuildModel(const ProjectDescriptor& descriptor, SolutionGenerationModel& model, std::string& errorMessage, SolutionGenerationLayout outputLayout = SolutionGenerationLayout::ProjectFilesV2) const;
	bool BuildCurrentModel(const std::filesystem::path& descriptorPath, ProjectDescriptor& descriptor,
		SolutionGenerationModel& model, std::string& errorMessage) const;

	ProjectRegistry& registry_;
	std::vector<ProjectSolutionPreviewSnapshot> previews_;
	std::vector<ProjectSolutionRecoverySnapshot> recoveries_;
};
