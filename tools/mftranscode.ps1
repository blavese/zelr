# Windows' own media codecs, for tools/genaac.py: a WAV into an M4A by
# Windows' AAC encoder, or anything Windows can read back into a WAV by its
# decoders. A host tool; nothing it makes runs on the machine.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/mftranscode.ps1 -In a.wav -Out a.m4a [-Quality Medium] [-Channels 2] [-Rate 44100]
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/mftranscode.ps1 -In a.m4a -Out a.wav [-Channels 2] [-Rate 44100]
#
# Give the rate and channels: Windows' profiles otherwise resample to theirs.
param([string]$In, [string]$Out, [string]$Quality = "Medium", [int]$Channels = 0, [int]$Rate = 0)
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Media.Transcoding.MediaTranscoder, Windows.Media.Transcoding, ContentType = WindowsRuntime]
$null = [Windows.Media.MediaProperties.MediaEncodingProfile, Windows.Media.MediaProperties, ContentType = WindowsRuntime]
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
$asTaskP = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncActionWithProgress`1' })[0]
function Await($op, [Type]$t) { $task = $asTask.MakeGenericMethod($t).Invoke($null, @($op)); $task.Wait(-1) | Out-Null; $task.Result }

$inPath = (Resolve-Path $In).Path
$outDir = Split-Path -Parent ([IO.Path]::GetFullPath($Out))
$outName = Split-Path -Leaf $Out
$src = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($inPath)) ([Windows.Storage.StorageFile])
$folder = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($outDir)) ([Windows.Storage.StorageFolder])
$dst = Await ($folder.CreateFileAsync($outName, [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])
$q = [Windows.Media.MediaProperties.AudioEncodingQuality]::$Quality
if ($Out.ToLower().EndsWith(".m4a")) {
    $profile = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateM4a($q)
} else {
    $profile = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateWav([Windows.Media.MediaProperties.AudioEncodingQuality]::High)
}
if ($Channels -gt 0) { $profile.Audio.ChannelCount = $Channels }
if ($Rate -gt 0) { $profile.Audio.SampleRate = $Rate }
$tc = New-Object Windows.Media.Transcoding.MediaTranscoder
$prep = Await ($tc.PrepareFileTranscodeAsync($src, $dst, $profile)) ([Windows.Media.Transcoding.PrepareTranscodeResult])
if (-not $prep.CanTranscode) { Write-Error ("cannot: " + $prep.FailureReason); exit 1 }
$t = $asTaskP.MakeGenericMethod([double]).Invoke($null, @($prep.TranscodeAsync()))
$t.Wait(-1) | Out-Null
exit 0
