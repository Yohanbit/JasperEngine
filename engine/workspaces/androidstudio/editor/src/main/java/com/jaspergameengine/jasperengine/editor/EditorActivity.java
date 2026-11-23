package com.jaspergameengine.jasperengine.editor;

import android.os.Build.VERSION;
import android.os.Build.VERSION_CODES;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.WindowManager.LayoutParams;

import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import com.google.androidgamesdk.GameActivity;
import com.jaspergameengine.jasperengine.JasperBootLog;

/**
 * The JasperEngine editor on Android.
 *
 * It extends GameActivity exactly as the game runtime's MainActivity does, so the same
 * libjasperengine-editor.so / android_main / native_app_glue machinery applies; what changes is
 * the library name and that this activity keeps the system bars visible instead of hiding them:
 * an editor's menus must not be swallowed by a fullscreen mode meant for games.
 *
 * Debug builds write Downloads/jasperengine-boot.txt (JasperBootLog) here too, so the editor can
 * be diagnosed on a phone without adb.
 */
public class EditorActivity extends GameActivity {

    private static volatile Throwable libraryLoadError;

    static {
        try {
            System.loadLibrary("jasperengine-editor");
        } catch (Throwable t) {
            // recorded, not thrown: a class whose initializer throws never reaches onCreate, so
            // nothing would be written to Downloads to say why the editor did not start
            libraryLoadError = t;
        }
    }

    private boolean isDebuggable() {
        return (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (isDebuggable()) JasperBootLog.install(this);
        if (libraryLoadError != null) {
            if (isDebuggable()) JasperBootLog.dump("library load failed: " + libraryLoadError, libraryLoadError);
            throw new RuntimeException("jasperengine-editor did not load: " + libraryLoadError, libraryLoadError);
        }

        // Where the native side keeps settings and projects: only Java can ask Android for it, and
        // it must be set before the first frame reads a setting
        EditorPaths.setProcessEnvironment(this);

        // What this APK is, for the boot report and the editor's own About/status: the release
        // name, build number and commit the release manager published (tools/android/releases.json)
        EditorPaths.setEnv("JASPERENGINE_ANDROID_RELEASE_NAME", EditorBuildInfo.RELEASE_NAME);
        EditorPaths.setEnv("JASPERENGINE_ANDROID_BUILD_NUMBER", String.valueOf(EditorBuildInfo.BUILD_NUMBER));
        EditorPaths.setEnv("JASPERENGINE_ANDROID_COMMIT", EditorBuildInfo.COMMIT);
        android.util.Log.i("JasperEngineEditor", "starting " + EditorBuildInfo.summary());

        // An editor session is long and a screen that dims mid-edit loses work in the user's hands
        getWindow().addFlags(LayoutParams.FLAG_KEEP_SCREEN_ON);

        // The editor draws behind the bars and keeps its own padding out of the cutout, but the
        // bars stay reachable: Back still leaves, and the editor's menus sit under them.
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        if (VERSION.SDK_INT >= VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                    LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        WindowInsetsControllerCompat controller =
                new WindowInsetsControllerCompat(getWindow(), getWindow().getDecorView());
        controller.show(WindowInsetsCompat.Type.systemBars());
        controller.setSystemBarsBehavior(
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);

        super.onCreate(savedInstanceState);
    }

    // -- the editor's clipboard (editor/backend/BackendAndroid.cpp -> ImGui's platform callbacks).
    // Java methods on the activity the native glue already holds, so no extra registration runs and
    // a call from the frame thread reaches the UI thread through runOnUiThread.

    /** ImGui's Platform_SetClipboardTextFn. A no-op before onCreate's Context is ready. */
    public void setEditorClipboardText(String text) {
        EditorClipboard.set(this, text);
    }

    /** ImGui's Platform_GetClipboardTextFn: "" when there is nothing (or no permission) to read. */
    public String getEditorClipboardText() {
        return EditorClipboard.get(this);
    }

    @Override
    protected void onStop() {
        super.onStop();
        // sharing the report is the only way off the phone without adb
        if (isDebuggable()) JasperBootLog.dumpAsync("editor activity stopped");
    }
}
