package com.harbourmasters.lighthouse;

import android.content.Intent;
import android.net.Uri;
import android.view.InputDevice;
import android.view.KeyEvent;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class LighthouseActivity extends SDLActivity {
    private static final int REQUEST_OOT_ROM = 0x7A1;

    private volatile String mOotRomDest;

    @Override
    protected String[] getLibraries() {
        // The game is built as libmain.so (see CMakeLists.txt)
        return new String[] { "SDL2", "main" };
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // The system Back button opens the menu like Esc on desktop, instead of closing the game.
        // Controllers that report a button as Back are left to SDL's controller handling.
        boolean fromController = (event.getSource() & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK)) != 0;
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && !fromController) {
            event = new KeyEvent(event.getDownTime(), event.getEventTime(), event.getAction(), KeyEvent.KEYCODE_ESCAPE,
                    event.getRepeatCount(), event.getMetaState(), event.getDeviceId(), event.getScanCode(),
                    event.getFlags(), event.getSource());
        }
        return super.dispatchKeyEvent(event);
    }

    /**
     * Called from native code (Play as Link's "Load Ocarina of Time ROM..."): lets the player pick the ROM with the
     * system file picker, which can reach Downloads where the in-game browser can't, and copies it to {@code dest}.
     * {@code dest + ".done"} appears once that's over, picked or not, and the game imports from there.
     */
    public void pickOotRom(String dest) {
        mOotRomDest = dest;
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            // .z64 has no registered MIME type
            intent.setType("*/*");
            try {
                startActivityForResult(intent, REQUEST_OOT_ROM);
            } catch (RuntimeException e) {
                finishOotRomPick(null);
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_OOT_ROM) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        finishOotRomPick(resultCode == RESULT_OK && data != null ? data.getData() : null);
    }

    private void finishOotRomPick(Uri uri) {
        final String dest = mOotRomDest;
        if (dest == null) {
            return;
        }
        new Thread(() -> {
            File destFile = new File(dest);
            destFile.delete();
            if (uri != null) {
                try (InputStream in = getContentResolver().openInputStream(uri);
                     OutputStream out = new FileOutputStream(destFile)) {
                    byte[] buffer = new byte[1 << 16];
                    int read;
                    while ((read = in.read(buffer)) > 0) {
                        out.write(buffer, 0, read);
                    }
                } catch (IOException | RuntimeException e) {
                    destFile.delete();
                }
            }
            try {
                new File(dest + ".done").createNewFile();
            } catch (IOException ignored) {
            }
        }).start();
    }
}
