package com.jaspergameengine.jasperengine;

import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.Process;
import android.provider.MediaStore;
import android.util.Log;

import androidx.core.content.FileProvider;

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
 * Writes what the app's start did to a file the user can find and share, without a PC, USB debugging
 * or the READ_LOGS permission: {@code Downloads/jasperengine-boot.txt}.
 *
 * <p>The engine logs to logcat under the tag "JasperEngine" (engine/core/log/sinks/AndroidLogSink),
 * and an app may read back the log of its own process. This gathers that, the device's identity, and
 * any fatal Java error, then writes it once the boot has had time to finish (a hang is evidence too)
 * and again when the activity stops, which is when the user goes looking for the file.
 *
 * <p>Debug builds only: MainActivity installs it under ApplicationInfo.FLAG_DEBUGGABLE, so an
 * exported release game ships none of it.
 */
public final class JasperBootLog {

	private static final String TAG = "JasperEngine";
	private static final String FILE_NAME = "jasperengine-boot.txt";
	/** the first frame is expected well before this */
	private static final long FIRST_DUMP_MS = 20_000L;
	private static final long SECOND_DUMP_MS = 60_000L;

	private static Context appContext;
	private static volatile boolean installed;
	/** the last report written, for the share sheet; null when writing failed */
	private static volatile Uri lastUri;

	private JasperBootLog() {
	}

	/** Installs once. Never throws: a report that cannot be written must not stop the game. */
	public static void install(Context context) {
		if (installed) return;
		synchronized (JasperBootLog.class) {
			if (installed) return;
			try {
				appContext = context.getApplicationContext();
				installed = true;
			} catch (Throwable t) {
				Log.w(TAG, "boot log: no context", t);
				return;
			}
		}
		try {
			installCrashHandler();
			Thread worker = new Thread(JasperBootLog::scheduleDumps, "JasperBootLog");
			worker.setDaemon(true);
			worker.start();
		} catch (Throwable t) {
			Log.w(TAG, "boot log: could not start", t);
		}
	}

	private static void scheduleDumps() {
		for (long delay : new long[] {FIRST_DUMP_MS, SECOND_DUMP_MS}) {
			try {
				Thread.sleep(delay);
			} catch (InterruptedException e) {
				return;
			}
			dump("after " + (delay / 1000) + "s");
		}
	}

	private static void installCrashHandler() {
		final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
		Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> {
			try {
				dump("fatal: " + throwable, throwable);
			} catch (Throwable ignored) {
				// the crash must still reach the system's handler, which shows the dialog and tombstone
			}
			if (previous != null) previous.uncaughtException(thread, throwable);
		});
	}

	/** dump() on a worker thread: for the UI thread, where reading logcat must not block. */
	public static void dumpAsync(final String reason) {
		Thread worker = new Thread(() -> dump(reason, null), "JasperBootLog-dump");
		worker.setDaemon(true);
		worker.start();
	}

	public static void dump(String reason) {
		dump(reason, null);
	}

	/** Builds the report, with the exception's stack trace in it. Never throws. */
	public static void dump(String reason, Throwable fatal) {
		if (appContext == null) return;
		try {
			Uri uri = writeFile(buildReport(reason, fatal));
			lastUri = uri;
			Log.i(TAG, "boot log: " + reason + " -> " + (uri == null ? "nowhere" : uri));
		} catch (Throwable t) {
			Log.w(TAG, "boot log: dump failed", t);
		}
	}

	/**
	 * Offers the last report to another app (an email, a chat, Drive). Called from the activity's
	 * back press when the game never drew: the way out of a hang that leaves no other trace.
	 */
	public static Intent shareIntent() {
		Uri uri = lastUri;
		if (uri == null) {
			// nothing written yet: make one now, the user is asking for it
			dump("shared by the user", null);
			uri = lastUri;
		}
		if (uri == null) return null;
		Intent send = new Intent(Intent.ACTION_SEND);
		send.setType("text/plain");
		send.putExtra(Intent.EXTRA_SUBJECT, "JasperEngine boot report");
		send.putExtra(Intent.EXTRA_STREAM, uri);
		send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
		return Intent.createChooser(send, "Share the boot report");
	}

	private static String buildReport(String reason, Throwable fatal) {
		StringBuilder out = new StringBuilder(64 * 1024);
		String stamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());

		out.append("JasperEngine boot report\n");
		out.append("when      : ").append(stamp).append('\n');
		out.append("reason    : ").append(reason).append('\n');
		out.append("package   : ").append(appContext.getPackageName()).append('\n');
		out.append("device    : ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append('\n');
		out.append("android   : ").append(Build.VERSION.RELEASE)
				.append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
		out.append("abis      : ").append(String.join(",", Build.SUPPORTED_ABIS)).append('\n');
		out.append("pid       : ").append(Process.myPid()).append('\n');
		out.append('\n');

		if (fatal != null) {
			out.append("=== fatal java exception ===\n");
			StringWriter trace = new StringWriter();
			fatal.printStackTrace(new PrintWriter(trace));
			out.append(trace).append('\n');
		}

		out.append("=== logcat for this process ===\n");
		out.append(readLogcat());
		out.append('\n');
		return out.toString();
	}

	/**
	 * logcat's own lines. An app may read the log of its own process; --pid narrows it, and the
	 * unfiltered form is the fallback for a logcat that refuses --pid (then ours is picked out).
	 */
	private static String readLogcat() {
		String pid = String.valueOf(Process.myPid());
		for (String[] command : new String[][] {
				{"logcat", "-d", "-v", "threadtime", "--pid=" + pid},
				{"logcat", "-d", "-v", "threadtime"}}) {
			StringBuilder chunk = new StringBuilder(32 * 1024);
			try {
				Process process = new ProcessBuilder(command).redirectErrorStream(true).start();
				try (BufferedReader reader = new BufferedReader(
						new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
					String line;
					while ((line = reader.readLine()) != null) {
						// ours by pid; the crash lines the system writes for us carry no such pid field
						if (command.length == 5 || line.contains(" " + pid + " ")
								|| line.contains(TAG) || line.contains("AndroidRuntime")
								|| line.contains("DEBUG")) {
							chunk.append(line).append('\n');
						}
					}
				}
				process.waitFor();
			} catch (Throwable t) {
				// A ROM whose exec of a system binary is refused: read it through a shell instead
				try {
					StringBuilder viaShell = new StringBuilder(32 * 1024);
					java.util.Scanner scanner = new java.util.Scanner(
							Runtime.getRuntime().exec("/system/bin/sh -c 'logcat -d -v threadtime'")
									.getInputStream(), StandardCharsets.UTF_8.name());
					scanner.useDelimiter("\\A");
					if (scanner.hasNext()) viaShell.append(scanner.next());
					scanner.close();
					if (viaShell.length() > 0) {
						chunk.append(viaShell);
					}
				} catch (Throwable alsoFailed) {
					chunk.append("(logcat ").append(String.join(" ", command)).append(" failed: ")
							.append(t).append("; through a shell: ").append(alsoFailed).append(")\n");
				}
			}
			if (chunk.length() > 0) return chunk.toString();
		}
		return "(no logcat lines: this ROM does not allow an app to read its own log)\n";
	}

	/**
	 * Downloads/ on API 29+ (MediaStore, no permission needed), else the app's own external files
	 * directory (no permission on any version, and visible over MTP). Returns the uri to share.
	 */
	private static Uri writeFile(String text) {
		byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
			try {
				ContentValues values = new ContentValues();
				values.put(MediaStore.Downloads.DISPLAY_NAME, FILE_NAME);
				values.put(MediaStore.Downloads.MIME_TYPE, "text/plain");
				values.put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS);
				values.put(MediaStore.Downloads.IS_PENDING, 1);
				Uri uri = appContext.getContentResolver()
						.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
				if (uri != null) {
					try (OutputStream stream = appContext.getContentResolver().openOutputStream(uri, "wt")) {
						if (stream != null) {
							stream.write(bytes);
							values.clear();
							values.put(MediaStore.Downloads.IS_PENDING, 0);
							appContext.getContentResolver().update(uri, values, null, null);
							return uri;
						}
					}
					appContext.getContentResolver().delete(uri, null, null);
				}
			} catch (Throwable t) {
				Log.w(TAG, "boot log: Downloads write failed", t);
			}
		}
		try {
			File dir = appContext.getExternalFilesDir(null);
			if (dir != null) {
				File file = new File(dir, FILE_NAME);
				try (FileOutputStream stream = new FileOutputStream(file)) {
					stream.write(bytes);
				}
				// the provider's authority is <applicationId>.bootlog (the manifest's FileProvider)
				return FileProvider.getUriForFile(appContext,
						appContext.getPackageName() + ".bootlog", file);
			}
		} catch (Throwable t) {
			Log.w(TAG, "boot log: external files write failed", t);
		}
		return null;
	}
}
