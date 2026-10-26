# 役割: 固定ツールの入力と完成binaryを照合し、必要時だけツールをBuildして生成APIへ渡す。
# ゲームのBuild/起動、依存Restore、生成規則の複製は行わない。
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Descriptor,
    [ValidateSet('Inspect', 'Ensure')][string]$Operation = 'Inspect',
    [string]$Registry,
    [string]$MSBuildPath,
    [switch]$Json
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$toolDirectory = [IO.Path]::GetFullPath($PSScriptRoot)
$toolWorkspace = [IO.Path]::GetFullPath((Join-Path $toolDirectory '..\..'))
$toolProject = Join-Path $toolDirectory 'ProjectGenerationTool.vcxproj'
$toolOutput = Join-Path $toolWorkspace 'generated\outputs\ProjectGenerationTool\Release'
$toolBinary = Join-Path $toolOutput 'ProjectGenerationTool.exe'
$toolStamp = Join-Path $toolOutput 'bootstrap.json'
$toolIntermediate = Join-Path $toolWorkspace 'generated\obj\ProjectGenerationTool\Release'
$toolFingerprintHeader = Join-Path $toolIntermediate 'InputFingerprint.h'
$toolJapanese = $true
$toolMutex = $null
$toolOwnsMutex = $false

function Select-ToolText([string]$Japanese, [string]$English) {
    if ($script:toolJapanese) { return $Japanese }
    return $English
}

function Assert-ToolPath([string]$Path) {
    $candidate = [IO.Path]::GetFullPath($Path)
    while ($candidate) {
        if (Test-Path -LiteralPath $candidate) {
            $entry = Get-Item -LiteralPath $candidate -Force
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw (Select-ToolText 'リンク経由のツール入出力には対応していません。実体のパスを使用してください。' 'Tool input/output through a reparse point is unsupported. Use the actual path.')
            }
        }
        $parent = [IO.Path]::GetDirectoryName($candidate)
        if ($parent -eq $candidate) { break }
        $candidate = $parent
    }
}

function Get-ToolInputFingerprint {
    [xml]$document = [IO.File]::ReadAllText($toolProject)
    $inputFiles = @($toolProject, $PSCommandPath)
    foreach ($node in $document.SelectNodes("//*[local-name()='ClCompile' or local-name()='ClInclude'][@Include]")) {
        $inputFiles += [IO.Path]::GetFullPath((Join-Path $toolDirectory $node.GetAttribute('Include')))
    }
    $records = foreach ($file in ($inputFiles | Sort-Object -Unique)) {
        if (-not $file.StartsWith($toolWorkspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw (Select-ToolText 'ツールの入力が作業ルートの外を参照しています。固定プロジェクトを確認してください。' 'A tool input is outside its workspace. Review the fixed bootstrap project.')
        }
        Assert-ToolPath $file
        $relative = $file.Substring($toolWorkspace.Length + 1).Replace('\', '/')
        $relative + ':' + (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    }
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes(($records -join "`n"))))).Replace('-', '') }
    finally { $hasher.Dispose() }
}

try {
    $descriptorPath = [IO.Path]::GetFullPath($Descriptor)
    $languagePath = Join-Path ([IO.Path]::GetDirectoryName($descriptorPath)) 'project\editor_settings.json'
    if (Test-Path -LiteralPath $languagePath -PathType Leaf) {
        try {
            $settings = [IO.File]::ReadAllText($languagePath) | ConvertFrom-Json
            if ($settings.PSObject.Properties['language']) { $toolJapanese = $settings.language -ne 'en' }
        } catch { $toolJapanese = $true }
    }
    Assert-ToolPath $toolProject
    Assert-ToolPath $toolBinary
    Assert-ToolPath $toolStamp
    Assert-ToolPath ($toolStamp + '.tmp')
    Assert-ToolPath $toolFingerprintHeader
    # 二つの入口が同時に古いtoolをBuildしない。ゲームProjectの排他はC++共通基盤が担当する。
    $keyHasher = [Security.Cryptography.SHA256]::Create()
    try { $key = ([BitConverter]::ToString($keyHasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($toolDirectory.ToUpperInvariant())))).Replace('-', '') }
    finally { $keyHasher.Dispose() }
    $toolMutex = [Threading.Mutex]::new($false, ('Global\CG3.ProjectGenerationBootstrap.' + $key))
    try { $toolOwnsMutex = $toolMutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $toolOwnsMutex = $true }
    if (-not $toolOwnsMutex) { throw (Select-ToolText '別の生成ツール操作が実行中です。完了してから再操作してください。' 'Another generation tool request is active. Retry after it finishes.') }

    $fingerprint = Get-ToolInputFingerprint
    $current = $false
    if ((Test-Path -LiteralPath $toolBinary -PathType Leaf) -and (Test-Path -LiteralPath $toolStamp -PathType Leaf)) {
        try {
            $stamp = [IO.File]::ReadAllText($toolStamp) | ConvertFrom-Json
            $current = $stamp.schemaVersion -eq 1 -and $stamp.fingerprint -eq $fingerprint -and $stamp.binaryHash -eq (Get-FileHash -LiteralPath $toolBinary -Algorithm SHA256).Hash
        } catch { $current = $false }
    }
    if (-not $current) {
        if (-not $MSBuildPath) {
            $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
            if (-not (Test-Path -LiteralPath $locator -PathType Leaf)) { throw (Select-ToolText 'Visual Studioを検出できません。MSBuildPathに既存のMSBuild.exeを指定してください。' 'Visual Studio could not be located. Supply an existing MSBuild.exe through MSBuildPath.') }
            $matches = @(& $locator -latest -products '*' -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\Current\Bin\MSBuild.exe')
            if ($LASTEXITCODE -ne 0 -or $matches.Count -ne 1) { throw (Select-ToolText 'C++用MSBuildを特定できません。既存のVisual Studio C++環境を確認してください。' 'C++ MSBuild could not be identified. Check the existing Visual Studio C++ installation.') }
            $MSBuildPath = $matches[0]
        }
        if (-not (Test-Path -LiteralPath $MSBuildPath -PathType Leaf)) { throw (Select-ToolText '指定されたMSBuild.exeがありません。パスを確認してください。' 'The specified MSBuild.exe does not exist. Check its path.') }
        [Console]::Error.WriteLine((Select-ToolText '生成ツールを更新しています。ゲーム本体のビルドは行いません。' 'Updating the generation tool. The game itself will not be built.'))
        # fingerprint差をC++の実際の依存へ渡す。元ソースの時刻が古くても旧exeを採用しない。
        [void][IO.Directory]::CreateDirectory($toolIntermediate)
        Assert-ToolPath $toolFingerprintHeader
        [IO.File]::WriteAllText($toolFingerprintHeader, ('// Generated bootstrap identity' + [Environment]::NewLine + '#define CG_PROJECT_GENERATION_INPUT "' + $fingerprint + '"' + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
        & $MSBuildPath $toolProject /nologo /m:1 /t:Build /p:Configuration=Release /p:Platform=x64 /nodeReuse:false /verbosity:minimal | ForEach-Object { [Console]::Error.WriteLine($_) }
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $toolBinary -PathType Leaf)) { throw (Select-ToolText '生成ツールのビルドに失敗しました。上のエラーを解消して再操作してください。旧ツールでは続行しません。' 'The generation tool build failed. Resolve the errors above and retry. The old tool will not be used.') }
        if ((Get-ToolInputFingerprint) -ne $fingerprint) { throw (Select-ToolText 'ツールのビルド中に入力が変更されました。保存を終えてから再操作してください。' 'Tool inputs changed during compilation. Finish saving files and retry.') }
        Assert-ToolPath $toolBinary
        Assert-ToolPath $toolStamp
        Assert-ToolPath ($toolStamp + '.tmp')
        $stampData = @{ schemaVersion = 1; fingerprint = $fingerprint; binaryHash = (Get-FileHash -LiteralPath $toolBinary -Algorithm SHA256).Hash } | ConvertTo-Json
        [IO.File]::WriteAllText($toolStamp + '.tmp', $stampData, [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath ($toolStamp + '.tmp') -Destination $toolStamp -Force
    }
    # 生成規則はC++だけに置く。CLI引数は文字列コマンドへ結合せず個別に渡す。
    $toolArguments = @($(if ($Operation -eq 'Ensure') { '--ensure' } else { '--inspect' }), '--descriptor', $descriptorPath)
    if ($Registry) { $toolArguments += @('--registry', [IO.Path]::GetFullPath($Registry)) }
    if ($Json) { $toolArguments += '--json' }
    & $toolBinary @toolArguments
    $resultCode = $LASTEXITCODE
} catch {
    [Console]::Error.WriteLine((Select-ToolText '生成ツールを開始・継続できません。' 'The generation tool could not start or continue.'))
    [Console]::Error.WriteLine($_.Exception.Message)
    if ($Json) {
        @{ schemaVersion = 1; state = 'Failed'; code = 'BootstrapFailed'; message = (Select-ToolText '生成ツールの準備に失敗しました。' 'Generation tool preparation failed.'); remedy = (Select-ToolText '標準エラーの原因と対処を確認してください。' 'Review the cause and remedy on standard error.'); detail = $_.Exception.Message } | ConvertTo-Json -Compress
    }
    $resultCode = 3
} finally {
    if ($toolOwnsMutex) { $toolMutex.ReleaseMutex() }
    if ($null -ne $toolMutex) { $toolMutex.Dispose() }
}
exit $resultCode
