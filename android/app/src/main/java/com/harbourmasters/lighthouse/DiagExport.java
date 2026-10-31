package com.harbourmasters.lighthouse;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;

import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Copies the diagnostics a LIGHTHOUSE_GL_DIAG build wrote (files/diag, then removed) and the game logs
 * (files/logs, kept) to Download/Lighthouse-diag.
 */
final class DiagExport {
    private DiagExport() {
    }

    /** Returns the folder the files were copied to, or null if there was nothing to copy. */
    static String run(Context context, File dataDir) {
        File diag = new File(dataDir, "diag");
        File[] files = diag.listFiles();
        if (files == null || files.length == 0 || Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            return null;
        }
        File[] logs = new File(dataDir, "logs").listFiles();
        String folder = Environment.DIRECTORY_DOWNLOADS + "/Lighthouse-diag/"
                + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        ContentResolver resolver = context.getContentResolver();
        if (logs != null) {
            for (File log : logs) {
                copy(resolver, log, folder);
            }
        }
        for (File file : files) {
            copy(resolver, file, folder);
            file.delete();
        }
        diag.delete();
        return folder;
    }

    private static void copy(ContentResolver resolver, File file, String folder) {
        ContentValues values = new ContentValues();
        values.put(MediaStore.Downloads.DISPLAY_NAME, file.getName());
        values.put(MediaStore.Downloads.MIME_TYPE, file.getName().endsWith(".png") ? "image/png" : "text/plain");
        values.put(MediaStore.Downloads.RELATIVE_PATH, folder);
        Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
        if (uri == null) {
            return;
        }
        try (OutputStream out = resolver.openOutputStream(uri)) {
            if (out != null) {
                Files.copy(file.toPath(), out);
            }
        } catch (IOException ignored) {
            resolver.delete(uri, null, null);
        }
    }
}
