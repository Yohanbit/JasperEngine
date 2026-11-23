package com.jaspergameengine.jasperengine.editor;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.util.Log;

/**
 * The system clipboard, for the editor's text fields.
 *
 * <p>Dear ImGui keeps the clipboard behind two platform callbacks ({@code Platform_SetClipboardTextFn}
 * and {@code Platform_GetClipboardTextFn}); on Linux the backend answers them with the X11 selection,
 * on Windows with the Win32 clipboard. Android has no such API reachable from C++, and the editor is
 * a shared library loaded into this process, so the only way is a JNI call into this class, which is
 * what the engine's own runtime does for its HTML UI clipboard.
 *
 * <p>{@link #set} must run on the UI thread ({@code ClipboardManager} is not thread safe and its
 * writes are enforced to the foreground app since Android 10); {@link #get} is called from the
 * editor's frame thread while the app is foreground, which is the one case Android allows a read in.
 * A failure returns "" rather than throwing: a paste that finds nothing is not an error.
 */
public final class EditorClipboard {

    private static final String TAG = "JasperEngineEditor";

    private EditorClipboard() {}

    /** Posts the text to the UI thread; safe to call from the editor's frame thread. */
    static void set(final Context context, final String text) {
        final String clip = text != null ? text : "";
        android.app.Activity activity = activityOf(context);
        Runnable write = new Runnable() {
            @Override public void run() {
                try {
                    ClipboardManager manager =
                            (ClipboardManager) context.getSystemService(Context.CLIPBOARD_SERVICE);
                    if (manager != null) {
                        manager.setPrimaryClip(ClipData.newPlainText("text", clip));
                    }
                } catch (RuntimeException e) {
                    Log.e(TAG, "could not write the clipboard", e);
                }
            }
        };
        if (activity != null) activity.runOnUiThread(write);
        else write.run();
    }

    /** "" when there is no clip, or when the platform refuses the read. */
    static String get(Context context) {
        try {
            ClipboardManager manager =
                    (ClipboardManager) context.getSystemService(Context.CLIPBOARD_SERVICE);
            if (manager == null || !manager.hasPrimaryClip()) return "";
            ClipData clip = manager.getPrimaryClip();
            if (clip == null || clip.getItemCount() == 0) return "";
            CharSequence text = clip.getItemAt(0).coerceToText(context);
            return text != null ? text.toString() : "";
        } catch (RuntimeException e) {
            Log.e(TAG, "could not read the clipboard", e);
            return "";
        }
    }

    private static android.app.Activity activityOf(Context context) {
        return context instanceof android.app.Activity ? (android.app.Activity) context : null;
    }
}
