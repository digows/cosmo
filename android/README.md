# Android build

This project uses SDL's Android activity with Cosmo's existing CMake Android
target. The APK bundles Episode 1 shareware data and the Android-compatible
mods, including the MP3 music mod. Android's MediaCodec API decodes the MP3
tracks into the same PCM format used by the audio mixer.
The Android system Back button sends Escape to the game.
Smooth Gameplay is enabled by default on fresh Android installs. Change it in
the Mods menu. Existing installs keep their saved mod setting.

## Import Episodes 2 and 3

If you own the game data, select **Mad Scientist** or **Secret Lab** in the
episode launcher and choose **Choose files**. In Android's file picker, touch
and hold the matching `COSMO2.STN` or `COSMO3.STN`, tap the matching `.VOL`,
then tap **Select**. The game copies both files to app storage and starts the
episode. The imported episode stays available in the launcher.

## Build a debug APK

Install JDK 17 and the Android SDK with platform 36, NDK `30.0.16248370`, and
CMake `3.31.6`. Accept the Android SDK licenses. The build downloads Gradle,
the Android Gradle Plugin, SDL3, and other build dependencies on first use.

From the repository root in PowerShell, fetch the checked Episode 1 data first
if `gamedata/COSMO1.STN` and `gamedata/COSMO1.VOL` are absent:

```powershell
$env:SHAREWARE_URL = "https://archive.org/download/CosmosCosmicAdventure/CosmosCosmicAdventure-ForbiddenPlanet-Adventure1Of3V1.20sw1992apogeeSoftwareLtd.action.zip"
$env:STN_SHA256 = "55963b5331a7159fe6ca134e9ab807e59e6ef798057fc28a1081c1e6a273ff52"
$env:VOL_SHA256 = "f578a1b72423e83b363c5c019faac5b96124f39bc4a2ec4ee09cabc29f3a63fd"
& "C:\msys64\usr\bin\bash.exe" .github/scripts/fetch-shareware.sh gamedata
```

Set `ANDROID_SDK_ROOT` and `JAVA_HOME` to your local installations. If you have
the project's sibling `android-toolchain` folder, `android/build.ps1` loads its
environment automatically. Otherwise run Gradle directly:

```powershell
.\android\gradlew.bat --no-daemon -p .\android assembleDebug
```

The APK is written to `android/app/build/outputs/apk/debug/app-debug.apk`.
It supports `arm64-v8a` and `x86_64`.

This is a development build signed with the standard debug key. Android app
signing and store release configuration are separate follow-up work.
Touch controls are enabled by default on Android and can be changed in the launcher settings.
