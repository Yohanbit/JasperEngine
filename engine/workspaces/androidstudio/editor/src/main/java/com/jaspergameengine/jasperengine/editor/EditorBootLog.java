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
        // a start that hangs never reaches onStop: two timed dumps leave one on disk anyway
        scheduleDump(20_000, "20 s after start");
        scheduleDump(60_000, "60 s after start");
    }

    /** One line in the report, so the order of events survives even when logcat is noisy. */
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
        // Downloads through MediaStore (API 29+): visible to the user's Files app and to Drive,
        // with no storage permission and no adb
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            try {
                final Uri collection = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
                final String where = MediaStore.MediaColumns.DISPLAY_NAME + "=?";
                final String[] args = {FILE_NAME};
                int updated = appContext.getContentResolver().update(collection,
                        new ContentValues(), where, args);
                if (updated > 0) {
                    appContext.getContentResolver().delete(collection, where, args);
                }
                final ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME, FILE_NAME);
                values.put(MediaStore.MediaColumns.MIME_TYPE, "text/plain");
                final Uri item = appContext.getContentResolver().insert(collection, values);
                if (item != null) {
                    try (OutputStream stream = appContext.getContentResolver().openOutputStream(item, "wt")) {
                        if (stream != null) {
                            stream.write(text.getBytes(StandardCharsets.UTF_8));
                            Log.i(TAG, "report written to Downloads/" + FILE_NAME);
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
            try (FileOutputStream stream = new FileOutputStream(new File(dir, FILE_NAME))) {
                stream.write(text.getBytes(StandardCharsets.UTF_8));
            }
            Log.i(TAG, "report written to the app's external files directory");
        } catch (Exception e) {
            Log.e(TAG, "the report could not be written at all", e);
        }
    }
}
