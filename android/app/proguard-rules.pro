# this is necessary so that native functions can call Java ones using JNI
-keep class org.libsdl.app** { *; }
-keep class org.vita3k.emulator.NativeLib { *; }
# reached only via JNI FindClass from NativeLib.init - R8 cannot see that and stripped it from
# Release builds, silently killing the previous-exit-reason diagnostics
-keep class org.vita3k.emulator.AndroidDiagnostics { *; }
-keep class org.vita3k.emulator.Emulator { *; }
-keep class org.vita3k.emulator.EmuSurface { *; }
-keep class org.vita3k.emulator.overlay.** { *; }
-keep class org.vita3k.emulator.data.EmulatorConfig { *; }
-keep class org.vita3k.emulator.data.NativeAppInfo { *; }
-keep class org.vita3k.emulator.data.NativeImeState { *; }
-keep class org.vita3k.emulator.data.NativeTrophy { *; }
-keep class org.vita3k.emulator.data.NativeTrophyCollection { *; }
-keep class org.vita3k.emulator.data.NativeUser { *; }
-keep interface org.vita3k.emulator.data.InstallCallback { *; }
