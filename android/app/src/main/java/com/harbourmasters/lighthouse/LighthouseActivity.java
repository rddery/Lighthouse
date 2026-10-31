package com.harbourmasters.lighthouse;

import android.view.InputDevice;
import android.view.KeyEvent;

import org.libsdl.app.SDLActivity;

public class LighthouseActivity extends SDLActivity {
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
}
