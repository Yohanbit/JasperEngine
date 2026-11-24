package com.jaspergameengine.jasperengine.editor;

import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;
import android.util.Log;


import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * The editor's start-up report, written to Downloads/editor-boot.txt (debug builds only).
 *
 * <p>Why it exists: a phone has no adb here. Without it, a start that fails on the device says
 * nothing at all. It is modelled on the game runtime's JasperBootLog, and deliberately separate
 * from it: the editor's report names the editor's own paths and version, and this module must not
 * pull the runtime's MainActivity and lifecycle bridge into its build.
 *
 * <p>It reads the app's own logcat (an app may always read its own output, no READ_LOGS), so it
 * captures native log lines, Java exceptions and the ActivityManager's view of the start in one
 * file. Written at 20 s, at 60 s, and when the activity stops -- a hang still leaves a report.
 */
final class EditorBootLog {

    private static final String TAG = "JasperEngineEditor";
    private static final String FILE_NAME = "editor-boot.txt";

    private static Context appContext;
    /** The activity's window while it is alive, so the report can ask it whether it is immersive. */
    private static java.lang.ref.WeakReference<android.view.Window> currentWindow =
            new java.lang.ref.WeakReference<>(null);
    private static Thread.UncaughtExceptionHandler previousHandler;
    private static final StringBuilder events = new StringBuilder();

    private EditorBootLog() {}

    static void install(Context context) {
        appContext = context.getApplicationContext();
        note("editor activity created");
        previousHandler = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> {
            note("FATAL on " + thread.getName());
            dump("uncaught exception", throwable);
            if (previousHandler != null) previousHandler.uncaughtException(thread, throwable);
        });
        // a start that hangs never reaches onStop: timed dumps leave a report on disk anyway
        scheduleDump(5_000, "5 s after start");
        scheduleDump(15_000, "15 s after start");
        scheduleDump(30_000, "30 s after start");
        scheduleDump(60_000, "60 s after start");
        exportNativeCrashesAsync();
    }

    /** One line in the report, so the order of events survives even when logcat is noisy. */
    /** Called by the activity: the window is what knows whether the bars are hidden. */
    static void setWindow(android.view.Window window) {
        currentWindow = new java.lang.ref.WeakReference<>(window);
    }

    static void note(String message) {
        synchronized (events) {
            events.append(new SimpleDateFormat("HH:mm:ss.SSS", Locale.US).format(new Date()))
                  .append("  ").append(message).append('\n');
        }
        Log.i(TAG, message);
    }

    private static void scheduleDump(long delayMillis, String why) {
        final Thread thread = new Thread(() -> {
            try {
                Thread.sleep(delayMillis);
            } catch (InterruptedException e) {
                return;
            }
            dumpAsync(why);
        }, "boot-report-" + delayMillis);
        thread.setDaemon(true);
        thread.start();
    }

    static void dumpAsync(String why) {
        final Thread thread = new Thread(() -> dump(why, null), "boot-report-write");
        thread.setDaemon(true);
        thread.start();
    }

    static synchronized void dump(String why, Throwable fatal) {
        if (appContext == null) return;

        final StringBuilder report = new StringBuilder();
        report.append("JasperEngine editor -- start report\n");
        report.append("written: ").append(new Date()).append('\n');
        report.append("reason: ").append(why).append('\n');
        report.append('\n');

        report.append("== device ==\n");
        report.append("manufacturer: ").append(Build.MANUFACTURER).append('\n');
        report.append("model: ").append(Build.MODEL).append('\n');
        report.append("android: ").append(Build.VERSION.RELEASE)
              .append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        report.append("abis: ").append(String.join(", ", Build.SUPPORTED_ABIS)).append('\n');
        report.append("package: ").append(appContext.getPackageName()).append('\n');
        report.append('\n');

        // Whether the window is actually immersive, and at what size it ended up. Both are read
        // here rather than judged from a screenshot: "the bars are still there" was the report,
        // and this is the line that says whether the app asked for them to go.
        report.append("== the window ==\n");
        final android.view.Window w = currentWindow.get();
        if (w != null) {
            final android.view.View decor = w.getDecorView();
            final android.view.WindowInsets insets =
                    decor.getRootWindowInsets();
            report.append("decor: ").append(decor.getWidth()).append('x').append(decor.getHeight())
                  .append('\n');
            // WindowInsets.Type arrived in API 30 and this app runs on 24 up, so the older
            // per-side query is used below that: it answers the same question for these two bars
            // and keeps the report readable on every supported phone.
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                report.append("status bar visible: ")
                      .append(insets == null || insets.isVisible(
                              android.view.WindowInsets.Type.statusBars())).append('\n');
                report.append("navigation bar visible: ")
                      .append(insets == null || insets.isVisible(
                              android.view.WindowInsets.Type.navigationBars())).append('\n');
            } else if (insets != null) {
                report.append("status bar inset: ").append(insets.getSystemWindowInsetTop())
                      .append('\n');
                report.append("navigation bar inset: ").append(insets.getSystemWindowInsetBottom())
                      .append('\n');
            }
        } else {
            report.append("decor: (no window)\n");
        }
        report.append('\n');

        report.append("== the editor's directories ==\n");
        report.append("config: ").append(EditorPaths.configDirectory(appContext)).append('\n');
        report.append("projects: ").append(EditorPaths.projectsDirectory(appContext)).append('\n');
        report.append('\n');

        report.append("== events ==\n");
        synchronized (events) {
            report.append(events);
        }
        report.append('\n');

        if (fatal != null) {
            report.append("== the exception ==\n");
            final StringWriter trace = new StringWriter();
            fatal.printStackTrace(new PrintWriter(trace));
            report.append(trace).append('\n');
        }

        report.append("== logcat of this process ==\n");
        report.append(readLogcat());

        write(report.toString());
    }

    /** This process's logcat: an app always reads its own, with no permission. */
    private static String readLogcat() {
        final StringBuilder out = new StringBuilder();
        java.lang.Process process = null;
        try {
            process = new ProcessBuilder("logcat", "-d", "-v", "time",
                    "--pid=" + android.os.Process.myPid())
                    .redirectErrorStream(true).start();
            try (BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) out.append(line).append('\n');
            }
        } catch (Exception e) {
            out.append("(could not read logcat: ").append(e).append(")\n");
        } finally {
            if (process != null) process.destroy();
        }
        return out.toString();
    }

    private static void write(String text) {
        writeToDownloads(FILE_NAME, text);
    }

    private static void exportNativeCrashesAsync() {
        final Thread thread = new Thread(() -> {
            if (appContext == null) return;
            try {
                final StringBuilder crashReport = new StringBuilder();
                final File filesDir = appContext.getFilesDir();
                if (filesDir != null && filesDir.exists()) {
                    collectCrashFiles(filesDir, crashReport, 0);
                }
                if (crashReport.length() > 0) {
                    writeToDownloads("crash.txt", crashReport.toString());
                }
            } catch (Exception e) {
                Log.w(TAG, "exportNativeCrashesAsync failed", e);
            }
        }, "crash-report-export");
        thread.setDaemon(true);
        thread.start();
    }

    private static void collectCrashFiles(File dir, StringBuilder out, int depth) {
        if (depth > 4 || dir == null || !dir.isDirectory()) return;
        final File[] children = dir.listFiles();
        if (children == null) return;
        for (File f : children) {
            if (f.isDirectory()) {
                collectCrashFiles(f, out, depth + 1);
            } else {
                final String name = f.getName();
                if (name.equals("crash.txt") || name.equals("report.txt") || name.equals("summary.txt")) {
                    out.append("=== ").append(f.getAbsolutePath()).append(" ===\n");
                    appendFileTail(f, out, 64 * 1024);
                    out.append("\n\n");
                }
            }
        }
    }

    private static void appendFileTail(File f, StringBuilder out, int maxBytes) {
        try (java.io.FileInputStream fis = new java.io.FileInputStream(f)) {
            long len = f.length();
            if (len > maxBytes) {
                //noinspection ResultOfMethodCallIgnored
                fis.skip(len - maxBytes);
                out.append("...(truncated)...\n");
            }
            byte[] buf = new byte[4096];
            int n;
            while ((n = fis.read(buf)) > 0) {
                out.append(new String(buf, 0, n, StandardCharsets.UTF_8));
            }
        } catch (Exception e) {
            out.append("(error reading file: ").append(e).append(")\n");
        }
    }

    private static void writeToDownloads(String fileName, String text) {
        // Downloads through MediaStore (API 29+): visible to the user's Files app and to Drive,
        // with no storage permission and no adb
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            try {
                final Uri collection = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
                final String where = MediaStore.MediaColumns.DISPLAY_NAME + "=?";
                final String[] args = {fileName};
                int updated = appContext.getContentResolver().update(collection,
                        new ContentValues(), where, args);
                if (updated > 0) {
                    appContext.getContentResolver().delete(collection, where, args);
                }
                final ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME, fileName);
                values.put(MediaStore.MediaColumns.MIME_TYPE, "text/plain");
                final Uri item = appContext.getContentResolver().insert(collection, values);
                if (item != null) {
                    try (OutputStream stream = appContext.getContentResolver().openOutputStream(item, "wt")) {
                        if (stream != null) {
                            stream.write(text.getBytes(StandardCharsets.UTF_8));
                            Log.i(TAG, "report written to Downloads/" + fileName);
                            return;
                        }
                    }
                }
            } catch (Exception e) {
                Log.w(TAG, "MediaStore write failed, falling back to the app directory", e);
            }
        }
        // the app's own external files directory: a FileProvider share reaches it (boot_log_paths)
        try {
            final File dir = appContext.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS);
            if (dir == null) return;
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
            try (FileOutputStream stream = new FileOutputStream(new File(dir, fileName))) {
                stream.write(text.getBytes(StandardCharsets.UTF_8));
            }
            Log.i(TAG, "report written to the app's external files directory/" + fileName);
        } catch (Exception e) {
            Log.e(TAG, "the report could not be written at all", e);
        }
    }
}
