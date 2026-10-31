package com.harbourmasters.lighthouse;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.provider.MediaStore;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.DateFormat;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Builds a text report about the last time the game crashed, so testers without adb can share it.
 * Uses the system's record of why the previous process ended (Android 11+), which includes the
 * native backtrace on Android 12+, plus this app's recent logcat output and the game's own log.
 */
final class CrashReport {
    private static final String PREFS = "crash-report";
    private static final String LAST_REPORTED = "last-shown-crash-timestamp";
    private static final int MAX_REPORT_CHARS = 200_000;
    private static final int MAX_TRACE_LINES = 3_000;

    private CrashReport() {
    }

    /** Returns a report for a crash that has not been reported yet, or null. */
    static String takeUnreported(Context context, File dataDir) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return null;
        }
        ActivityManager am = context.getSystemService(ActivityManager.class);
        List<ApplicationExitInfo> exits = am.getHistoricalProcessExitReasons(null, 0, 1);
        if (exits.isEmpty()) {
            return null;
        }
        ApplicationExitInfo exit = exits.get(0);
        int reason = exit.getReason();
        if (reason != ApplicationExitInfo.REASON_CRASH && reason != ApplicationExitInfo.REASON_CRASH_NATIVE
                && reason != ApplicationExitInfo.REASON_ANR) {
            return null;
        }
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (prefs.getLong(LAST_REPORTED, 0) >= exit.getTimestamp()) {
            return null;
        }
        prefs.edit().putLong(LAST_REPORTED, exit.getTimestamp()).apply();

        StringBuilder report = new StringBuilder();
        report.append("Lighthouse ").append(versionName(context)).append(" crash report\n");
        report.append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(", Android ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT)
                .append(")\n");
        report.append("Build: ").append(Build.FINGERPRINT).append('\n');
        report.append("Exit: ").append(reasonName(reason)).append(", status ").append(exit.getStatus())
                .append(", at ").append(DateFormat.getDateTimeInstance().format(new Date(exit.getTimestamp())))
                .append('\n');
        if (exit.getDescription() != null) {
            report.append("Description: ").append(exit.getDescription()).append('\n');
        }

        report.append("\n== Crash trace ==\n").append(trace(exit));
        report.append("\n== Game log ==\n").append(tail(new File(dataDir, "logs/Lighthouse.log"), 200));
        report.append("\n== Logcat ==\n").append(logcat());

        if (report.length() > MAX_REPORT_CHARS) {
            report.setLength(MAX_REPORT_CHARS);
            report.append("\n[truncated]\n");
        }
        return report.toString();
    }

    /**
     * Saves the report to the public Download folder, which the Files app can open (unlike the app's own
     * Android/data folder). Returns the file's content URI, which can be shared, or null if it could not be saved.
     */
    static Uri saveToDownloads(Context context, String report) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            return null;
        }
        String name = "Lighthouse-crash-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date())
                + ".txt";
        ContentResolver resolver = context.getContentResolver();
        ContentValues values = new ContentValues();
        values.put(MediaStore.Downloads.DISPLAY_NAME, name);
        values.put(MediaStore.Downloads.MIME_TYPE, "text/plain");
        values.put(MediaStore.Downloads.IS_PENDING, 1);
        Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
        if (uri == null) {
            return null;
        }
        try (OutputStream out = resolver.openOutputStream(uri)) {
            if (out == null) {
                throw new IOException("Cannot open " + uri);
            }
            out.write(report.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            resolver.delete(uri, null, null);
            return null;
        }
        values.clear();
        values.put(MediaStore.Downloads.IS_PENDING, 0);
        resolver.update(uri, values, null, null);
        return uri;
    }

    /** The name the saved report has in the Download folder. */
    static String displayName(Context context, Uri uri) {
        try (Cursor cursor = context.getContentResolver().query(uri,
                new String[] { MediaStore.Downloads.DISPLAY_NAME }, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                return cursor.getString(0);
            }
        }
        return "";
    }

    private static String trace(ApplicationExitInfo exit) {
        try (InputStream in = exit.getTraceInputStream()) {
            if (in == null) {
                return "(none recorded)\n";
            }
            byte[] bytes = readAll(in);
            if (exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE) {
                // A tombstone protobuf: its text fields (backtrace symbols, libraries, abort message) are enough.
                // The backtraces come before the long memory map, which is cut off.
                return printableStrings(bytes, 4, MAX_TRACE_LINES);
            }
            return new String(bytes, StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "(unreadable: " + e.getMessage() + ")\n";
        }
    }

    private static String logcat() {
        // Apps can read their own log lines, including those from the process that crashed
        try {
            Process process = new ProcessBuilder("logcat", "-d", "-t", "400", "-v", "threadtime")
                    .redirectErrorStream(true).start();
            try (InputStream in = process.getInputStream()) {
                return new String(readAll(in), StandardCharsets.UTF_8);
            }
        } catch (IOException e) {
            return "(unavailable: " + e.getMessage() + ")\n";
        }
    }

    private static String tail(File file, int lines) {
        if (!file.exists()) {
            return "(no log)\n";
        }
        ArrayDeque<String> last = new ArrayDeque<>();
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(Files.newInputStream(file.toPath()), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (last.size() == lines) {
                    last.removeFirst();
                }
                last.addLast(line);
            }
        } catch (IOException e) {
            return "(unreadable: " + e.getMessage() + ")\n";
        }
        return String.join("\n", last) + "\n";
    }

    private static String printableStrings(byte[] bytes, int minLength, int maxLines) {
        StringBuilder out = new StringBuilder();
        int start = -1;
        int lines = 0;
        for (int i = 0; i <= bytes.length && lines < maxLines; i++) {
            boolean printable = i < bytes.length && bytes[i] >= 0x20 && bytes[i] < 0x7f;
            if (printable && start < 0) {
                start = i;
            } else if (!printable && start >= 0) {
                if (i - start >= minLength) {
                    out.append(new String(bytes, start, i - start, StandardCharsets.US_ASCII)).append('\n');
                    lines++;
                }
                start = -1;
            }
        }
        return out.toString();
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[1 << 16];
        int read;
        while ((read = in.read(buffer)) != -1) {
            out.write(buffer, 0, read);
        }
        return out.toByteArray();
    }

    private static String reasonName(int reason) {
        switch (reason) {
            case ApplicationExitInfo.REASON_CRASH:
                return "Java crash";
            case ApplicationExitInfo.REASON_CRASH_NATIVE:
                return "native crash";
            case ApplicationExitInfo.REASON_ANR:
                return "not responding (ANR)";
            default:
                return "reason " + reason;
        }
    }

    private static String versionName(Context context) {
        try {
            return context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "?";
        }
    }
}
