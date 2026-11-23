package com.jaspergameengine.jasperengine.editor;

import android.content.Context;

import java.io.File;

/**
 * The directories the editor's C++ side needs, which only Java can ask Android for.
 *
 * <p>The native code reads these from the environment (see editor/AppSettings.cpp and
 * editor/Platform.cpp); this class writes them there before the editor's first frame, because a
 * phone has no HOME and current_path() is "/" -- an app cannot create anything there.
 */
final class EditorPaths {

    private EditorPaths() {}

    /**
     * Where settings, the Project Hub's journal and the projects an editor session creates live.
     *
     * <p>getFilesDir() is private to the app and always writable, and it survives an app update.
     * It is NOT the user's Downloads: reaching those needs the Storage Access Framework, which is
     * asynchronous and answered by an activity, not something a filesystem path can express.
     */
    static File configDirectory(Context context) {
        return new File(context.getFilesDir(), "config");
    }

    /** A project the editor makes from a template when the user asks for a new one. */
    static File projectsDirectory(Context context) {
        return new File(context.getFilesDir(), "projects");
    }

    static void setProcessEnvironment(Context context) {
        final File config = configDirectory(context);
        //noinspection ResultOfMethodCallIgnored
        config.mkdirs();
        final File projects = projectsDirectory(context);
        //noinspection ResultOfMethodCallIgnored
        projects.mkdirs();
        setEnv("JASPERENGINE_ANDROID_CONFIG_DIR", config.getAbsolutePath());
        setEnv("JASPERENGINE_ANDROID_PROJECTS_DIR", projects.getAbsolutePath());
    }

    /**
     * setenv for this process. Android keeps the C library's environment, and the native code
     * reads it with std::getenv, so this needs no help from the platform.
     */
    static void setEnv(String name, String value) {
        try {
            android.system.Os.setenv(name, value, true);
        } catch (Throwable t) {
            // older levels may refuse; the native fallbacks (getFilesDir is not reachable from
            // C++ without JNI, so a clear failure beats a silent one in the wrong directory)
            android.util.Log.e("JasperEngineEditor", "could not set " + name, t);
        }
    }
}
