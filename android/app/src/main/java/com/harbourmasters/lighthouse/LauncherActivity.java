package com.harbourmasters.lighthouse;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * Prepares the app's external files directory (where libultraship looks for everything on Android)
 * before the game starts:
 *  - copies lighthouse.o2r, config.yml and the extraction yamls bundled in the APK, once per install/update
 *  - if the game has not been extracted yet, asks for the ROM with the system file picker and copies it
 *    there, where the game's first-run extractor finds it
 */
public class LauncherActivity extends Activity {
    private static final int REQUEST_ROM = 1;
    private static final String BUNDLED_DATA = "data";
    private static final String STAMP_FILE = ".bundled-data-version";

    private File mDataDir;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mDataDir = getExternalFilesDir(null);

        new Thread(() -> {
            try {
                copyBundledDataIfNeeded();
            } catch (IOException e) {
                runOnUiThread(() -> fail(e));
                return;
            }
            runOnUiThread(() -> {
                if (needsRom()) {
                    pickRom();
                } else {
                    startGame();
                }
            });
        }).start();
    }

    private void copyBundledDataIfNeeded() throws IOException {
        String version;
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            // Changes on every install or update, so new builds always refresh the bundled files
            version = Long.toString(info.lastUpdateTime);
        } catch (PackageManager.NameNotFoundException e) {
            throw new IOException(e);
        }

        File stamp = new File(mDataDir, STAMP_FILE);
        if (stamp.exists() && version.equals(new String(Files.readAllBytes(stamp.toPath()), StandardCharsets.UTF_8))) {
            return;
        }

        copyAssetTree(getAssets(), BUNDLED_DATA, mDataDir);
        Files.write(stamp.toPath(), version.getBytes(StandardCharsets.UTF_8));
    }

    private static void copyAssetTree(AssetManager assets, String path, File dest) throws IOException {
        String[] children = assets.list(path);
        if (children == null || children.length == 0) {
            try (InputStream in = assets.open(path)) {
                copy(in, dest);
            }
            return;
        }
        if (!dest.isDirectory() && !dest.mkdirs()) {
            throw new IOException("Cannot create " + dest);
        }
        for (String child : children) {
            copyAssetTree(assets, path + "/" + child, new File(dest, child));
        }
    }

    private static void copy(InputStream in, File dest) throws IOException {
        File tmp = new File(dest.getPath() + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            byte[] buffer = new byte[1 << 16];
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
        }
        if (!tmp.renameTo(dest)) {
            throw new IOException("Cannot write " + dest);
        }
    }

    private boolean needsRom() {
        if (new File(mDataDir, "bk.o2r").exists()) {
            return false;
        }
        File[] roms = mDataDir.listFiles((dir, name) -> name.toLowerCase().endsWith(".z64"));
        return roms == null || roms.length == 0;
    }

    private void pickRom() {
        Toast.makeText(this, R.string.select_rom, Toast.LENGTH_LONG).show();
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        // .z64 has no registered MIME type
        intent.setType("*/*");
        startActivityForResult(intent, REQUEST_ROM);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_ROM) {
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            // The game's in-app file browser is still available
            startGame();
            return;
        }

        Uri uri = data.getData();
        Toast.makeText(this, R.string.copying_rom, Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) {
                    throw new IOException("Cannot open " + uri);
                }
                copy(in, new File(mDataDir, romFileName(uri)));
            } catch (IOException e) {
                runOnUiThread(() -> fail(e));
                return;
            }
            runOnUiThread(this::startGame);
        }).start();
    }

    private String romFileName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null,
                null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                String name = new File(cursor.getString(0)).getName();
                if (name.toLowerCase().endsWith(".z64")) {
                    return name;
                }
            }
        }
        // The extractor only scans for .z64 files and validates the ROM itself
        return "baserom.z64";
    }

    private void fail(IOException e) {
        Toast.makeText(this, getString(R.string.copy_failed, e.getMessage()), Toast.LENGTH_LONG).show();
        finish();
    }

    private void startGame() {
        startActivity(new Intent(this, LighthouseActivity.class));
        finish();
    }
}
