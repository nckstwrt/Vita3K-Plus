package org.vita3k.emulator

import android.app.Application
import android.util.Log
import org.libsdl.app.SDLActivity
import org.vita3k.emulator.data.UiLanguages

class Vita3KApplication : Application() {
    companion object {
        @JvmStatic
        var instance: Vita3KApplication? = null
            private set
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        System.loadLibrary("Vita3K")
        SDLActivity.nativeSetenv("SDL_ANDROID_ALLOW_RECREATE_ACTIVITY", "1")
        UiLanguages.applyStored(this)
        installUncaughtExceptionLogger()
    }

    private fun installUncaughtExceptionLogger() {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            try {
                val text = "JAVA CRASH in thread '${thread.name}': ${Log.getStackTraceString(throwable)}"
                Log.e("Vita3K", text)
                if (NativeLib.isInitialized())
                    NativeLib.logDiagnostics(text)
            } catch (ignored: Throwable) {
            }
            previous?.uncaughtException(thread, throwable)
        }
    }
}
