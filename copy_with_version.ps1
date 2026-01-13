param (
    [string]$SourceFile,
    [string]$TargetDir,
    [string]$BaseName
)

$versionFile = "$TargetDir\version.txt"

if (Test-Path $versionFile) {
    $version = [int](Get-Content $versionFile)
    $version++
} else {
    $version = 1
}


Set-Content $versionFile $version

$verStr = "{0:D3}" -f $version
$targetFile = "$TargetDir\$BaseName" + "_v$verStr.exe"

Copy-Item $SourceFile $targetFile -Force
Write-Output "Copied to $targetFile"
