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

/**
 * The JasperEngine editor on Android.
 *
 * It extends GameActivity exactly as the game runtime's MainActivity does, so the same
 * libjasperengine-editor.so / android_main / native_app_glue machinery applies; what changes is
 * the library name and that this activity keeps the system bars visible instead of hiding them:
 * an editor's menus must not be swallowed by a fullscreen mode meant for games.
 *
 * Debug builds write Downloads/editor-boot.txt (EditorBootLog) so the editor can be diagnosed on a
 * phone without adb.
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
        if (isDebuggable()) {
            EditorBootLog.install(this);
            EditorBootLog.setWindow(getWindow());
        }
        if (libraryLoadError != null) {
            if (isDebuggable()) EditorBootLog.dump("library load failed: " + libraryLoadError, libraryLoadError);
            throw new RuntimeException("jasperengine-editor did not load: " + libraryLoadError, libraryLoadError);
        }

        // Where the native side keeps settings and projects: only Java can ask Android for it, and
        // it must be set before the first frame reads a setting
        EditorPaths.setProcessEnvironment(this);

        // An editor session is long and a screen that dims mid-edit loses work in the user's hands
        getWindow().addFlags(LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Immersive. The status bar was covering the editor's own menu row on a 2400x1080 phone
        // and the navigation bar was covering the bottom of the dockspace, and hiding them is
        // worth far more than the two rows of pixels the bars occupy: an editor is a full-screen
        // tool, and this is how every Android drawing and CAD app behaves.
        //
        // BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE is what keeps it recoverable without a visible
        // bar: a swipe from the top edge brings the status bar (and with it the shade) back for a
        // few seconds, a swipe from the bottom brings the navigation bar. Nothing is lost -- the
        // app switch and the home gesture keep working over the swipe -- and the first tap puts
        // the screen back to full.
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        if (VERSION.SDK_INT >= VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                    LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        WindowInsetsControllerCompat controller =
                new WindowInsetsControllerCompat(getWindow(), getWindow().getDecorView());
        controller.hide(WindowInsetsCompat.Type.systemBars());
        controller.setSystemBarsBehavior(
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);

        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onStop() {
        super.onStop();
        // sharing the report is the only way off the phone without adb
        if (isDebuggable()) EditorBootLog.dumpAsync("editor activity stopped");
    }
}
