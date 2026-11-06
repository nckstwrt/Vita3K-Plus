package org.vita3k.emulator;

import android.content.Context;
import android.content.Intent;
import android.content.res.Configuration;
import android.content.res.AssetFileDescriptor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.ViewGroup;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.RelativeLayout;
import android.widget.Toast;

import androidx.annotation.Keep;
import androidx.compose.ui.platform.ComposeView;
import androidx.core.app.ActivityCompat;
import androidx.core.content.pm.ShortcutInfoCompat;
import androidx.core.content.pm.ShortcutManagerCompat;
import androidx.core.graphics.drawable.IconCompat;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import androidx.lifecycle.Lifecycle;
import androidx.lifecycle.LifecycleOwner;
import androidx.lifecycle.LifecycleRegistry;
import androidx.lifecycle.ViewModelProvider;
import androidx.lifecycle.ViewModelStore;
import androidx.lifecycle.ViewModelStoreOwner;
import androidx.lifecycle.ViewTreeLifecycleOwner;
import androidx.savedstate.SavedStateRegistry;
import androidx.savedstate.SavedStateRegistryController;
import androidx.savedstate.SavedStateRegistryOwner;
import androidx.savedstate.ViewTreeSavedStateRegistryOwner;
import androidx.lifecycle.ViewTreeViewModelStoreOwner;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;
import org.libsdl.app.SDLDummyEdit;
import org.libsdl.app.SDLSurface;
import org.vita3k.emulator.data.AppStorage;
import org.vita3k.emulator.data.NativeImeState;
import org.vita3k.emulator.overlay.InputOverlay;
import org.vita3k.emulator.overlay.OverlayLayout;
import org.vita3k.emulator.overlay.OverlayStore;
import org.vita3k.emulator.ui.screens.emulation.NativeImeOverlayHost;
import org.vita3k.emulator.ui.screens.emulation.EmulationPauseMenuHost;
import org.vita3k.emulator.ui.viewmodel.EmulationSessionViewModel;
import org.vita3k.emulator.ui.viewmodel.SettingsViewModel;

public class Emulator extends SDLActivity
        implements InputManager.InputDeviceListener, LifecycleOwner, SavedStateRegistryOwner, ViewModelStoreOwner
{
    private static final String TAG = "Vita3K";
    private static final long IME_RESTORE_DELAY_MS = 250L;
    public static final String EXTRA_TITLE_ID = "title_id";
    public static final String EXTRA_GAME_TITLE = "game_title";
    private static final String APP_RESTART_PARAMETERS = "AppStartParameters";
    static final int FILE_DIALOG_CODE = 545;
    static final int FOLDER_DIALOG_CODE = 546;
    static final int FOLDER_ACCESS_SETTINGS_CODE = 547;
    static final int FOLDER_ACCESS_PERMISSION_CODE = 548;
    public interface ImagePathResultCallback {
        void onResult(String path);
    }
    private final LifecycleRegistry lifecycleRegistry = new LifecycleRegistry(this);
    private final SavedStateRegistryController savedStateRegistryController =
            SavedStateRegistryController.create(this);
    private final ViewModelStore composeViewModelStore = new ViewModelStore();
    private String currentGameId = "";
    private EmuSurface mSurface;
    private ComposeView imeOverlayView;
    private ComposeView pauseMenuView;
    private EmulationSessionViewModel sessionViewModel;
    private SettingsViewModel pauseSettingsViewModel;
    private SettingsViewModel pauseGlobalSettingsViewModel;
    private InputManager inputManager;
    private boolean nativeKeyboardRequested;
    private boolean imeDismissedByUser;
    private boolean imeVisible;
    private boolean imeWasVisibleSinceRequest;
    private boolean suppressImeHiddenHandler;
    private boolean restoreImeAfterPauseMenu;
    private long lastImeCompletionUptimeMs;
    private long lastUnfocusedControllerLogMs;
    private String lastAndroidPads;
    private int lastOverlayMask = -1;
    private final HashMap<Integer, DirectPad> directPads = new HashMap<>();
    private RelativeLayout.LayoutParams lastTextInputLayoutParams;
    private int lastTextInputType;
    private boolean hasLastTextInputState;
    private ImagePathResultCallback pendingImagePathCallback;

    public static Intent createLaunchIntent(Context context, String titleId, String gameTitle) {
        Intent intent = new Intent(context, Emulator.class);
        if (titleId != null && !titleId.isEmpty()) {
            intent.putExtra(EXTRA_TITLE_ID, titleId);
        }
        if (gameTitle != null && !gameTitle.isEmpty()) {
            intent.putExtra(EXTRA_GAME_TITLE, gameTitle);
        }
        intent.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return intent;
    }

    public InputOverlay getmOverlay() {
        return mSurface != null ? mSurface.getmOverlay() : null;
    }

    @Keep
    public void setCurrentGameId(String gameId){
        runOnUiThread(() -> {
            currentGameId = gameId != null ? gameId : "";
            clearDirectPads("a game is starting");
            logInputDevices("launch of " + currentGameId);
            refreshControllerOverlayScope();
            if (sessionViewModel != null) {
                String runningAppTitle = "";
                try {
                    runningAppTitle = NativeLib.INSTANCE.getRunningAppTitle();
                } catch (Throwable ignored) {
                }
                sessionViewModel.updateRunningApp(currentGameId, runningAppTitle);
            }
            releaseControllerOverlayInputs();
            refreshUiState(true);
        });
    }

    /**
     * This method is called by SDL before loading the native shared libraries.
     * It can be overridden to provide names of shared libraries to be loaded.
     * The default implementation returns the defaults. It never returns null.
     * An array returned by a new implementation must at least contain "SDL2".
     * Also keep in mind that the order the libraries are loaded may matter.
     *
     * @return names of shared libraries to be loaded (e.g. "SDL3", "main").
     */
    @Override
    protected String[] getLibraries() {
        return new String[] { "Vita3K" };
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        mSurface = new EmuSurface(context);
        mSurface.post(() -> {
            if (mSurface.getParent() instanceof ViewGroup) {
                InputOverlay overlay = getmOverlay();
                if (overlay != null && overlay.getParent() == null) {
                    ((ViewGroup) mSurface.getParent()).addView(overlay);
                }
                refreshControllerOverlayScope();
                refreshUiState(true);
            }
        });
        return mSurface;
    }

    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();

        // Check for restart parameters first (used by in-process relaunches)
        String[] args = intent.getStringArrayExtra(APP_RESTART_PARAMETERS);
        if (args != null && args.length > 0)
            return args;

        // Check for title_id from MainActivity launch
        String titleId = intent.getStringExtra(EXTRA_TITLE_ID);
        if (titleId != null && !titleId.isEmpty())
            return new String[]{"-r", titleId};

        return new String[]{};
    }

    @Override
    protected void onNewIntent(Intent intent){
        super.onNewIntent(intent);
        final Intent currentIntent = getIntent();
        setIntent(intent);
        if (!hasLaunchRequest(intent))
            return;

        if (isSameLaunchRequest(currentIntent, intent) && NativeLib.INSTANCE.isAppRunning())
            return;

        requestRelaunchFromIntent(intent);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (!ensureNativeSessionInitialized()) {
            finish();
            return;
        }
        savedStateRegistryController.performAttach();
        savedStateRegistryController.performRestore(savedInstanceState);
        lifecycleRegistry.handleLifecycleEvent(Lifecycle.Event.ON_CREATE);
        prepareComposeHostTree();
        setWindowStyle(true);
        applyImmersiveMode();
        refreshNativeDisplayRotation();

        currentGameId = resolveCurrentTitleId(getIntent());
        String gameTitle = getIntent().getStringExtra(EXTRA_GAME_TITLE);
        sessionViewModel = new EmulationSessionViewModel(getApplication());
        sessionViewModel.initialize(currentGameId, gameTitle);
        inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (inputManager != null) {
            inputManager.registerInputDeviceListener(this, null);
        }
        updateControllerConnectionState();
        refreshControllerOverlayScope();
        syncOverlayFromSession();
        installImeOverlay();
        installPauseMenu();
        installKeyboardVisibilityListener();
        refreshUiState(false);
    }

    @Override
    protected void onResume() {
        super.onResume();
        lifecycleRegistry.handleLifecycleEvent(Lifecycle.Event.ON_RESUME);
        refreshUiState(false);
        refreshNativeDisplayRotation();
        updateControllerConnectionState();
        resumeFromBackgroundIfNeeded();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        refreshNativeDisplayRotation();
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        try {
            if (NativeLib.INSTANCE.isInitialized())
                NativeLib.INSTANCE.onTrimMemory(level);
        } catch (Throwable ignored) {
        }
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        try {
            if (NativeLib.INSTANCE.isInitialized())
                NativeLib.INSTANCE.onTrimMemory(80);
        } catch (Throwable ignored) {
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        lifecycleRegistry.handleLifecycleEvent(Lifecycle.Event.ON_START);
    }

    @Override
    protected void onPause() {
        clearDirectPads("the app paused");
        suspendForBackgroundIfNeeded();
        lifecycleRegistry.handleLifecycleEvent(Lifecycle.Event.ON_PAUSE);
        super.onPause();
    }

    @Override
    protected void onStop() {
        lifecycleRegistry.handleLifecycleEvent(Lifecycle.Event.ON_STOP);
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        if (inputManager != null) {
            inputManager.unregisterInputDeviceListener(this);
        }
        lifecycleRegistry.handleLifecycleEvent(Lifecycle.Event.ON_DESTROY);
        composeViewModelStore.clear();
        super.onDestroy();
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        savedStateRegistryController.performSave(outState);
        super.onSaveInstanceState(outState);
    }

    @Override
    public void onBackPressed() {
        if (sessionViewModel != null && sessionViewModel.handleBackPressed(this))
            return;

        super.onBackPressed();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            refreshUiState(false);
            focusGameSurface();
        } else {
            clearDirectPads("the game window lost focus");
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        updateDirectPadKey(event);
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && nativeKeyboardRequested) {
            if (event.getAction() == KeyEvent.ACTION_UP && event.getRepeatCount() == 0) {
                dismissImeFromKeyboard(mTextEdit);
            }
            return true;
        }

        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_UP
                    && event.getRepeatCount() == 0
                    && sessionViewModel != null
                    && sessionViewModel.handleBackPressed(this)) {
                return true;
            }
            return true;
        }

        if (event.getKeyCode() == KeyEvent.KEYCODE_BUTTON_MODE
                && event.getAction() == KeyEvent.ACTION_DOWN
                && event.getRepeatCount() == 0) {
            openPauseMenuFromController();
            return true;
        }

        if (event.getAction() == KeyEvent.ACTION_DOWN && isControllerSource(event.getSource())
                && mSurface != null && !mSurface.hasFocus() && gameOwnsControllerInput()) {
            reclaimGameFocus("key " + KeyEvent.keyCodeToString(event.getKeyCode()), event.getDeviceId());
        }

        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        updateDirectPadMotion(event);
        if (isControllerSource(event.getSource()) && mSurface != null && !mSurface.hasFocus()) {
            if (gameOwnsControllerInput()) {
                reclaimGameFocus("stick", event.getDeviceId());
            } else if (nativeKeyboardRequested && mTextEdit != null && mTextEdit.hasFocus()) {
                // the keyboard's text box passes keys on to SDL but not sticks
                return mSurface.dispatchGenericMotionEvent(event);
            }
        }
        return super.dispatchGenericMotionEvent(event);
    }

    private static boolean isControllerSource(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    // no menu, dialog or keyboard of ours is on screen to take controller input
    private boolean gameOwnsControllerInput() {
        return mSurface != null && !isFinishing() && !nativeKeyboardRequested
                && (pauseMenuView == null || pauseMenuView.getVisibility() != View.VISIBLE)
                && (imeOverlayView == null || imeOverlayView.getVisibility() != View.VISIBLE);
    }

    private void reclaimGameFocus(String what, int deviceId) {
        long now = SystemClock.uptimeMillis();
        if (now - lastUnfocusedControllerLogMs > 5000L) {
            lastUnfocusedControllerLogMs = now;
            logInputDiagnostic("[INPUT] controller " + what + " from device " + deviceId + " while " + describeView(getCurrentFocus())
                    + " had focus, focus returned to the game");
        }
        mSurface.requestFocus();
    }

    public void focusGameSurface() {
        runOnUiThread(() -> {
            if (mSurface == null || isFinishing() || nativeKeyboardRequested || mSurface.hasFocus())
                return;
            if (sessionViewModel != null && (sessionViewModel.getUiState().getShowMenu()
                    || sessionViewModel.getUiState().isEditingControls()
                    || sessionViewModel.getUiState().getShowExitConfirmation()))
                return;
            mSurface.requestFocus();
        });
    }

    // called by the native loop (SDL's own rescan stalls after an SDL re-init)
    @Keep
    public void pollControllerDevices() {
        SDLControllerManager.pollInputDevices();
    }

    private String describeView(View view) {
        if (view == null)
            return "nothing";
        if (view == mSurface)
            return "the game";
        if (view == mTextEdit)
            return "the keyboard text box";
        if (isInside(view, pauseMenuView))
            return "the pause menu";
        if (isInside(view, imeOverlayView))
            return "the keyboard overlay";
        return view.getClass().getSimpleName();
    }

    private static boolean isInside(View view, View container) {
        if (container == null)
            return false;
        for (Object v = view; v instanceof View; v = ((View) v).getParent()) {
            if (v == container)
                return true;
        }
        return false;
    }

    private static final int[] PAD_KEYS = {
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2,
            KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT,
            KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_MENU, KeyEvent.KEYCODE_BUTTON_C, KeyEvent.KEYCODE_BUTTON_Z,
            KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_CENTER,
            KeyEvent.KEYCODE_BUTTON_1, KeyEvent.KEYCODE_BUTTON_2, KeyEvent.KEYCODE_BUTTON_3, KeyEvent.KEYCODE_BUTTON_4,
            KeyEvent.KEYCODE_BUTTON_5, KeyEvent.KEYCODE_BUTTON_6, KeyEvent.KEYCODE_BUTTON_7, KeyEvent.KEYCODE_BUTTON_8,
            KeyEvent.KEYCODE_BUTTON_9, KeyEvent.KEYCODE_BUTTON_10, KeyEvent.KEYCODE_BUTTON_11, KeyEvent.KEYCODE_BUTTON_12,
            KeyEvent.KEYCODE_BUTTON_13, KeyEvent.KEYCODE_BUTTON_14, KeyEvent.KEYCODE_BUTTON_15, KeyEvent.KEYCODE_BUTTON_16,
            KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_SPACE, KeyEvent.KEYCODE_ESCAPE, KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_Z
    };

    private static String describeSources(int sources) {
        final int[] values = { InputDevice.SOURCE_KEYBOARD, InputDevice.SOURCE_DPAD, InputDevice.SOURCE_GAMEPAD, InputDevice.SOURCE_JOYSTICK,
                InputDevice.SOURCE_TOUCHSCREEN, InputDevice.SOURCE_MOUSE, InputDevice.SOURCE_MOUSE_RELATIVE, InputDevice.SOURCE_STYLUS,
                InputDevice.SOURCE_TRACKBALL, InputDevice.SOURCE_TOUCHPAD, InputDevice.SOURCE_TOUCH_NAVIGATION, InputDevice.SOURCE_ROTARY_ENCODER,
                InputDevice.SOURCE_HDMI, InputDevice.SOURCE_SENSOR };
        final String[] names = { "keyboard", "dpad", "gamepad", "joystick", "touchscreen", "mouse", "mouse-relative", "stylus",
                "trackball", "touchpad", "touch-navigation", "rotary", "hdmi", "sensor" };
        StringBuilder text = new StringBuilder(String.format(Locale.ROOT, "0x%08X (", sources));
        int named = 0;
        for (int i = 0; i < values.length; i++) {
            if ((sources & values[i]) == values[i])
                text.append(named++ == 0 ? "" : " ").append(names[i]);
        }
        return text.append(named == 0 ? "none)" : ")").toString();
    }

    private static int sdlGuidCrc(InputDevice device) {
        String desc = device.getDescriptor();
        if (desc == null || desc.isEmpty())
            desc = device.getName();
        int crc = 0;
        for (byte b : desc.getBytes(java.nio.charset.StandardCharsets.UTF_8)) {
            int r = (crc ^ b) & 0xFF;
            int c = 0;
            for (int i = 0; i < 8; i++) {
                c = (((c ^ r) & 1) != 0 ? 0xA001 : 0) ^ (c >>> 1);
                r >>>= 1;
            }
            crc = c ^ (crc >>> 8);
        }
        return crc & 0xFFFF;
    }

    private static String describeDevice(InputDevice device) {
        StringBuilder text = new StringBuilder(String.format(Locale.ROOT, "device %d '%s' vendor 0x%04X product 0x%04X | sources %s, keyboard type %d, controller number %d",
                device.getId(), device.getName(), device.getVendorId(), device.getProductId(), describeSources(device.getSources()),
                device.getKeyboardType(), device.getControllerNumber()));
        if (Build.VERSION.SDK_INT >= 29)
            text.append(device.isExternal() ? ", external" : ", built in");
        if (device.isVirtual())
            text.append(", virtual");
        text.append(String.format(Locale.ROOT, " | SDL joystick %s, SDL GUID crc 0x%04X, descriptor %s",
                SDLControllerManager.isDeviceSDLJoystick(device.getId()) ? "yes" : "no", sdlGuidCrc(device), device.getDescriptor()));
        boolean[] has = device.hasKeys(PAD_KEYS);
        text.append(" | keys:");
        int keys = 0;
        for (int i = 0; i < PAD_KEYS.length; i++) {
            if (has[i]) {
                text.append(' ').append(KeyEvent.keyCodeToString(PAD_KEYS[i]).replace("KEYCODE_", ""));
                keys++;
            }
        }
        if (keys == 0)
            text.append(" none");
        text.append(" | axes:");
        List<InputDevice.MotionRange> ranges = device.getMotionRanges();
        if (ranges.isEmpty())
            text.append(" none");
        for (InputDevice.MotionRange range : ranges) {
            text.append(String.format(Locale.ROOT, " %s(0x%X %.2f..%.2f flat %.2f)", MotionEvent.axisToString(range.getAxis()).replace("AXIS_", ""),
                    range.getSource(), range.getMin(), range.getMax(), range.getFlat()));
        }
        return text.toString();
    }

    private void logInputDevices(String why) {
        int[] ids = InputDevice.getDeviceIds();
        StringBuilder text = new StringBuilder(String.format(Locale.ROOT, "[INPUT] %d input devices (%s):", ids.length, why));
        for (int id : ids) {
            InputDevice device = InputDevice.getDevice(id);
            text.append("\n[INPUT] ").append(device != null ? describeDevice(device) : "device " + id + " (gone)");
        }
        logInputDiagnostic(text.toString());
    }

    private void logInputDeviceChange(String what, int deviceId) {
        InputDevice device = InputDevice.getDevice(deviceId);
        logInputDiagnostic("[INPUT] input device " + what + ": " + (device != null ? describeDevice(device) : "device " + deviceId));
    }

    private static final int DIRECT_PAD_LEFT_TRIGGER = 1 << 30;
    private static final int DIRECT_PAD_RIGHT_TRIGGER = 1 << 31;
    private static final int[] DIRECT_PAD_ZRZ = { MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ };
    private static final int[] DIRECT_PAD_RXRY = { MotionEvent.AXIS_RX, MotionEvent.AXIS_RY };

    private static final class DirectPad {
        int keys;
        int hat;
        final float[] axes = new float[6];
        int crc;
        boolean axesChosen;
        int[] stick;
        int[] triggers;
        final boolean[] triggerRestsLow = new boolean[2];
        int sentButtons = -1;
        final float[] sentAxes = new float[6];
    }

    private static native void setDirectPad(int deviceId, String name, int vendor, int product, int crc, int buttons, float[] axes);
    private static native void removeDirectPad(int deviceId);

    private static boolean isDirectPadDevice(InputDevice device) {
        if (device == null || device.isVirtual())
            return false;
        final String name = device.getName() != null ? device.getName() : "";
        if (name.startsWith("uinput-") || name.startsWith("gf_"))
            return false;
        final int sources = device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    // the SDL gamepad button SDL gives each key (BACK and MODE open the pause menu instead)
    private static int directPadKeyBit(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_DPAD_CENTER:
                return 1;
            case KeyEvent.KEYCODE_BUTTON_B:
                return 1 << 1;
            case KeyEvent.KEYCODE_BUTTON_X:
                return 1 << 2;
            case KeyEvent.KEYCODE_BUTTON_Y:
                return 1 << 3;
            case KeyEvent.KEYCODE_BUTTON_SELECT:
                return 1 << 4;
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_MENU:
                return 1 << 6;
            case KeyEvent.KEYCODE_BUTTON_THUMBL:
                return 1 << 7;
            case KeyEvent.KEYCODE_BUTTON_THUMBR:
                return 1 << 8;
            case KeyEvent.KEYCODE_BUTTON_L1:
                return 1 << 9;
            case KeyEvent.KEYCODE_BUTTON_R1:
                return 1 << 10;
            case KeyEvent.KEYCODE_DPAD_UP:
                return 1 << 11;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                return 1 << 12;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                return 1 << 13;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return 1 << 14;
            case KeyEvent.KEYCODE_BUTTON_L2:
                return DIRECT_PAD_LEFT_TRIGGER;
            case KeyEvent.KEYCODE_BUTTON_R2:
                return DIRECT_PAD_RIGHT_TRIGGER;
            default:
                return 0;
        }
    }

    private DirectPad directPadFor(InputDevice device) {
        if (nativeKeyboardRequested) {
            clearDirectPads("the keyboard is up");
            return null;
        }
        if (!isDirectPadDevice(device))
            return null;
        DirectPad pad = directPads.get(device.getId());
        if (pad == null) {
            pad = new DirectPad();
            pad.crc = sdlGuidCrc(device);
            directPads.put(device.getId(), pad);
        }
        return pad;
    }

    private void updateDirectPadKey(KeyEvent event) {
        final int bit = directPadKeyBit(event.getKeyCode());
        if (bit == 0 || (event.getFlags() & KeyEvent.FLAG_FALLBACK) != 0)
            return;
        final InputDevice device = event.getDevice();
        final DirectPad pad = directPadFor(device);
        if (pad == null)
            return;
        if (event.getAction() == KeyEvent.ACTION_DOWN)
            pad.keys |= bit;
        else if (event.getAction() == KeyEvent.ACTION_UP)
            pad.keys &= ~bit;
        sendDirectPad(device, pad);
    }

    private static boolean hasAxis(InputDevice device, int axis) {
        return device.getMotionRange(axis, InputDevice.SOURCE_JOYSTICK) != null;
    }

    private static int restsLow(MotionEvent event, int axis) {
        return event.getAxisValue(axis) <= -0.9f ? 1 : 0;
    }

    private static boolean triggerRestsLow(InputDevice device, MotionEvent event, int axis) {
        final float value = event.getAxisValue(axis);
        if (value <= -0.5f)
            return true;
        if (Math.abs(value) < 0.5f)
            return false;
        final InputDevice.MotionRange range = device.getMotionRange(axis, InputDevice.SOURCE_JOYSTICK);
        return range != null && range.getMin() < -0.5f;
    }

    private static String axisPairName(int[] axes) {
        return axes == null ? "none" : MotionEvent.axisToString(axes[0]).replace("AXIS_", "") + "/" + MotionEvent.axisToString(axes[1]).replace("AXIS_", "");
    }

    // the pair with more axes resting at -1 holds the triggers (Z/RZ and RX/RY are the right stick on some pads and the triggers on others)
    private void chooseDirectPadAxes(InputDevice device, DirectPad pad, MotionEvent event) {
        pad.axesChosen = true;
        final boolean zrz = hasAxis(device, MotionEvent.AXIS_Z) && hasAxis(device, MotionEvent.AXIS_RZ);
        final boolean rxry = hasAxis(device, MotionEvent.AXIS_RX) && hasAxis(device, MotionEvent.AXIS_RY);
        final int zLow = restsLow(event, MotionEvent.AXIS_Z) + restsLow(event, MotionEvent.AXIS_RZ);
        final int rLow = restsLow(event, MotionEvent.AXIS_RX) + restsLow(event, MotionEvent.AXIS_RY);
        int[] pairTriggers = null;
        if (zrz && rxry) {
            pad.stick = zLow > rLow ? DIRECT_PAD_RXRY : DIRECT_PAD_ZRZ;
            pairTriggers = zLow > rLow ? DIRECT_PAD_ZRZ : DIRECT_PAD_RXRY;
        } else if (zrz) {
            if (zLow == 2)
                pairTriggers = DIRECT_PAD_ZRZ;
            else
                pad.stick = DIRECT_PAD_ZRZ;
        } else if (rxry) {
            if (rLow == 2)
                pairTriggers = DIRECT_PAD_RXRY;
            else
                pad.stick = DIRECT_PAD_RXRY;
        }
        if (hasAxis(device, MotionEvent.AXIS_LTRIGGER) && hasAxis(device, MotionEvent.AXIS_RTRIGGER))
            pad.triggers = new int[] { MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER };
        else if (hasAxis(device, MotionEvent.AXIS_BRAKE) && hasAxis(device, MotionEvent.AXIS_GAS))
            pad.triggers = new int[] { MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS };
        else
            pad.triggers = pairTriggers;
        if (pad.triggers != null) {
            for (int i = 0; i < 2; i++)
                pad.triggerRestsLow[i] = triggerRestsLow(device, event, pad.triggers[i]);
        }
        logInputDiagnostic(String.format(Locale.ROOT, "[INPUT] direct pad: device %d '%s' right stick %s, triggers %s%s | first values Z=%.2f RZ=%.2f RX=%.2f RY=%.2f LTRIGGER=%.2f RTRIGGER=%.2f BRAKE=%.2f GAS=%.2f",
                device.getId(), device.getName(), axisPairName(pad.stick), axisPairName(pad.triggers),
                pad.triggers != null && (pad.triggerRestsLow[0] || pad.triggerRestsLow[1]) ? " (travel -1..1)" : "",
                event.getAxisValue(MotionEvent.AXIS_Z), event.getAxisValue(MotionEvent.AXIS_RZ), event.getAxisValue(MotionEvent.AXIS_RX),
                event.getAxisValue(MotionEvent.AXIS_RY), event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                event.getAxisValue(MotionEvent.AXIS_BRAKE), event.getAxisValue(MotionEvent.AXIS_GAS)));
    }

    private void updateDirectPadMotion(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) == 0 || event.getActionMasked() != MotionEvent.ACTION_MOVE)
            return;
        final InputDevice device = event.getDevice();
        final DirectPad pad = directPadFor(device);
        if (pad == null)
            return;
        if (!pad.axesChosen)
            chooseDirectPadAxes(device, pad, event);
        pad.axes[0] = event.getAxisValue(MotionEvent.AXIS_X);
        pad.axes[1] = event.getAxisValue(MotionEvent.AXIS_Y);
        pad.axes[2] = pad.stick != null ? event.getAxisValue(pad.stick[0]) : 0f;
        pad.axes[3] = pad.stick != null ? event.getAxisValue(pad.stick[1]) : 0f;
        for (int i = 0; i < 2; i++) {
            float value = pad.triggers != null ? event.getAxisValue(pad.triggers[i]) : 0f;
            if (pad.triggers != null && pad.triggerRestsLow[i])
                value = (value + 1f) / 2f;
            pad.axes[4 + i] = Math.max(0f, Math.min(1f, value));
        }
        final float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
        final float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        pad.hat = (hatY < -0.5f ? 1 << 11 : 0) | (hatY > 0.5f ? 1 << 12 : 0) | (hatX < -0.5f ? 1 << 13 : 0) | (hatX > 0.5f ? 1 << 14 : 0);
        sendDirectPad(device, pad);
    }

    private void sendDirectPad(InputDevice device, DirectPad pad) {
        final int buttons = pad.keys | pad.hat;
        boolean changed = buttons != pad.sentButtons;
        for (int i = 0; i < 6 && !changed; i++)
            changed = Math.abs(pad.axes[i] - pad.sentAxes[i]) >= 0.01f;
        if (!changed)
            return;
        pad.sentButtons = buttons;
        System.arraycopy(pad.axes, 0, pad.sentAxes, 0, 6);
        try {
            setDirectPad(device.getId(), device.getName(), device.getVendorId(), device.getProductId(), pad.crc, buttons, pad.axes);
        } catch (Throwable ignored) {
        }
    }

    private void clearDirectPads(String why) {
        if (directPads.isEmpty())
            return;
        directPads.clear();
        try {
            removeDirectPad(-1);
        } catch (Throwable ignored) {
        }
        logInputDiagnostic("[INPUT] direct pads cleared (" + why + ")");
    }

    private void dropDirectPad(int deviceId, String why) {
        if (directPads.remove(deviceId) == null)
            return;
        try {
            removeDirectPad(deviceId);
        } catch (Throwable ignored) {
        }
        logInputDiagnostic("[INPUT] direct pad: device " + deviceId + " dropped (" + why + ")");
    }

    private static void logInputDiagnostic(String text) {
        try {
            if (NativeLib.INSTANCE.isInitialized())
                NativeLib.INSTANCE.logDiagnostics(text);
        } catch (Throwable ignored) {
        }
    }

    @Keep
    public void restartApp(String app_path, String exec_path, String exec_args){
        final String[] guestArgs = exec_args != null && !exec_args.isEmpty()
                ? new String[]{exec_args}
                : new String[0];
        requestNativeRelaunch(app_path, exec_path, guestArgs, !exec_path.isEmpty());
    }

    @Keep
    public void setControllerOverlayState(int overlay_mask, boolean edit, boolean reset){
        InputOverlay overlay = getmOverlay();
        if (overlay == null)
            return;

        if (overlay_mask != lastOverlayMask) {
            logInputDiagnostic(String.format(Locale.ROOT, "[INPUT] on-screen controls mask %s -> 0x%X (editing %s, pad connected %s, hidden while a pad is connected %s)",
                    lastOverlayMask < 0 ? "unset" : String.format(Locale.ROOT, "0x%X", lastOverlayMask), overlay_mask, edit,
                    sessionViewModel != null && sessionViewModel.getUiState().getControllerConnected(),
                    sessionViewModel != null && sessionViewModel.getUiState().getHideOverlayWhenControllerConnected()));
            lastOverlayMask = overlay_mask;
        }
        overlay.setState(overlay_mask);
        overlay.setIsInEditMode(edit);

        if(reset)
            overlay.resetButtonPlacement();
    }

    @Keep
    public void setControllerOverlayLayout(OverlayLayout layout) {
        InputOverlay overlay = getmOverlay();
        if (overlay != null && layout != null)
            overlay.setLayout(layout);
    }

    private int mSavedOverlayMask = 0;

    @Keep
    public void setKeyboardActive(boolean active) {
        runOnUiThread(() -> {
            nativeKeyboardRequested = active;
            if (active) {
                clearDirectPads("the keyboard opened");
                imeDismissedByUser = false;
                restoreImeAfterPauseMenu = false;
                suppressImeHiddenHandler = false;
                imeWasVisibleSinceRequest = imeVisible;
            } else {
                imeDismissedByUser = false;
                restoreImeAfterPauseMenu = false;
                suppressImeHiddenHandler = false;
                imeWasVisibleSinceRequest = false;
            }
            applyKeyboardOverlayState(active);
            if (active) {
                scheduleVitaTextEditSwap();
            }
            if (active) {
                ensureOverlayUiOrder();
            }
        });
    }

    public void updateNativeImeState(boolean sceImeActive,
            boolean dialogActive,
            String text,
            int preeditStart,
            int preeditLength,
            int caretIndex,
            boolean multiline,
            String enterLabel) {
        runOnUiThread(() -> {
            if (sessionViewModel == null) {
                return;
            }

            sessionViewModel.updateImeState(new NativeImeState(
                    sceImeActive,
                    dialogActive,
                    text != null ? text : "",
                    preeditStart,
                    preeditLength,
                    caretIndex,
                    multiline,
                    enterLabel != null ? enterLabel : ""));
        });
    }

    public void clearNativeImeState() {
        runOnUiThread(() -> {
            if (sessionViewModel != null) {
                sessionViewModel.updateImeState(null);
            }
        });
    }

    @Keep
    public void setControllerOverlayScale(float scale){
        InputOverlay overlay = getmOverlay();
        if (overlay != null)
            overlay.setScale(scale);
    }

    @Keep
    public void setControllerOverlayOpacity(int opacity){
        InputOverlay overlay = getmOverlay();
        if (overlay != null)
            overlay.setOpacity(opacity);
    }

    public void releaseControllerOverlayInputs() {
        InputOverlay overlay = getmOverlay();
        if (overlay != null)
            overlay.releaseAllInputs();
    }

    public OverlayLayout captureControllerOverlayLayout() {
        InputOverlay overlay = getmOverlay();
        if (overlay != null)
            return overlay.captureLayout();

        return OverlayStore.defaultLayout(this);
    }

    public void refreshControllerOverlayScope() {
        InputOverlay overlay = getmOverlay();
        if (overlay != null) {
            overlay.refreshOverlayScope(currentGameId);
        }
    }

    public void requestNativeQuit() {
        runOnUiThread(() -> {
            try {
                NativeLib.INSTANCE.requestAppQuit();
            } catch (Throwable ignored) {
            }
            if (!isFinishing()) {
                finish();
            }
        });
    }

    public void ensurePauseMenuOnTop() {
        ensureOverlayUiOrder();
    }

    public void ensureOverlayUiOrder() {
        if (imeOverlayView != null) {
            imeOverlayView.bringToFront();
        }
        if (pauseMenuView != null) {
            pauseMenuView.bringToFront();
        }
    }

    public void prepareImeForPauseMenu() {
        runOnUiThread(() -> {
            if (nativeKeyboardRequested && !imeDismissedByUser) {
                restoreImeAfterPauseMenu = true;
                suppressImeHiddenHandler = true;
            }
        });
    }

    public void restoreImeAfterPauseMenu() {
        runOnUiThread(() -> {
            if (!restoreImeAfterPauseMenu) {
                suppressImeHiddenHandler = false;
                return;
            }

            restoreImeAfterPauseMenu = false;
            suppressImeHiddenHandler = false;
            if (!nativeKeyboardRequested || imeDismissedByUser || imeVisible || isFinishing()) {
                return;
            }

            ensureOverlayUiOrder();
            View anchor = mSurface != null ? mSurface : getWindow().getDecorView();
            anchor.postDelayed(() -> {
                if (nativeKeyboardRequested
                        && !imeDismissedByUser
                        && !imeVisible
                        && sessionViewModel != null
                        && !sessionViewModel.getUiState().getShowMenu()
                        && !sessionViewModel.getUiState().isEditingControls()) {
                    restoreVitaTextInput();
                }
            }, 32L);
        });
    }

    public void completeImeFromKeyboard(View source) {
        runOnUiThread(() -> {
            if (!nativeKeyboardRequested || imeDismissedByUser) {
                return;
            }

            long now = SystemClock.uptimeMillis();
            if (now - lastImeCompletionUptimeMs < 150L) {
                return;
            }
            lastImeCompletionUptimeMs = now;

            imeDismissedByUser = true;
            restoreImeAfterPauseMenu = false;
            suppressImeHiddenHandler = true;
            applyKeyboardOverlayState(false);

            View target = source != null ? source : (mSurface != null ? mSurface : getWindow().getDecorView());
            InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.hideSoftInputFromWindow(target.getWindowToken(), 0);
            }
            target.clearFocus();
            if (mTextEdit instanceof VitaTextEdit) {
                ((VitaTextEdit) mTextEdit).clearConnectionState();
            }
            if (mSurface != null) {
                mSurface.requestFocus();
            }

            try {
                NativeLib.INSTANCE.submitIme();
            } catch (Throwable ignored) {
            }
        });
    }

    public void dismissImeFromKeyboard(View source) {
        runOnUiThread(() -> {
            if (!nativeKeyboardRequested || isFinishing()) {
                return;
            }

            long now = SystemClock.uptimeMillis();
            if (now - lastImeCompletionUptimeMs < 150L) {
                return;
            }
            lastImeCompletionUptimeMs = now;

            View target = source != null ? source : (mSurface != null ? mSurface : getWindow().getDecorView());
            InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.hideSoftInputFromWindow(target.getWindowToken(), 0);
            }
            target.clearFocus();
            if (mTextEdit instanceof VitaTextEdit) {
                ((VitaTextEdit) mTextEdit).clearConnectionState();
            }
            if (mSurface != null) {
                mSurface.requestFocus();
            }

            NativeImeState imeState = sessionViewModel != null ? sessionViewModel.getImeState() : null;
            boolean dialogActive = imeState != null && imeState.getDialogActive();

            boolean dismissedInNative = false;
            try {
                dismissedInNative = NativeLib.INSTANCE.dismissIme();
            } catch (Throwable ignored) {
            }

            restoreImeAfterPauseMenu = false;
            suppressImeHiddenHandler = true;

            if (dismissedInNative && dialogActive) {
                imeDismissedByUser = true;
                applyKeyboardOverlayState(false);
                return;
            }

            imeDismissedByUser = false;

            if (sessionViewModel != null
                    && (sessionViewModel.getUiState().getShowMenu()
                    || sessionViewModel.getUiState().isEditingControls())) {
                restoreImeAfterPauseMenu = true;
                return;
            }

            scheduleImeRestoreCheck(target);
        });
    }

    private void scheduleImeRestoreCheck(View anchor) {
        View target = anchor != null ? anchor : (mSurface != null ? mSurface : getWindow().getDecorView());
        ensureOverlayUiOrder();
        target.postDelayed(() -> {
            NativeImeState imeState = sessionViewModel != null ? sessionViewModel.getImeState() : null;
            boolean imeStillActive = imeState != null
                    && (imeState.getSceImeActive() || imeState.getDialogActive());

            if (!nativeKeyboardRequested
                    || !imeStillActive
                    || imeVisible
                    || isFinishing()
                    || (sessionViewModel != null
                    && (sessionViewModel.getUiState().getShowMenu()
                    || sessionViewModel.getUiState().isEditingControls()))) {
                return;
            }

            restoreVitaTextInput();
            ensureOverlayUiOrder();
        }, IME_RESTORE_DELAY_MS);
    }

    public void openPauseMenuFromController() {
        runOnUiThread(() -> {
            if (sessionViewModel == null || isFinishing() || sessionViewModel.getUiState().getShowMenu()
                    || sessionViewModel.getUiState().isEditingControls()) {
                return;
            }

            ensurePauseMenuOnTop();
            sessionViewModel.openMenu(this, true);
        });
    }

    @Keep
    public static boolean createShortcut(Context context, String game_id, String game_name, File icon_file){
        if(context == null || game_id == null || game_id.isEmpty())
            return false;

        if(!ShortcutManagerCompat.isRequestPinShortcutSupported(context))
            return false;

        String shortcut_label = game_name != null && !game_name.isEmpty() ? game_name : game_id;

        // first look at the icon, its location should always be the same
        File src_icon = icon_file != null && icon_file.exists()
                ? icon_file
                : new File(context.getExternalFilesDir(null), "cache/icons/" + game_id + ".png");
        Bitmap icon;
        if(src_icon.exists())
            icon = BitmapFactory.decodeFile(src_icon.getPath());
        else
            icon = BitmapFactory.decodeResource(context.getResources(), R.mipmap.ic_launcher_plus);
        if(icon == null)
            icon = BitmapFactory.decodeResource(context.getResources(), R.mipmap.ic_launcher_plus);

        // intent to directly start the game
        Intent game_intent = createLaunchIntent(context, game_id, shortcut_label);
        game_intent.setAction("LAUNCH_" + game_id);

        // now create the pinned shortcut
        ShortcutInfoCompat shortcut = new ShortcutInfoCompat.Builder(context, game_id)
                .setShortLabel(shortcut_label)
                .setLongLabel(shortcut_label)
                .setIcon(IconCompat.createWithBitmap(icon))
                .setIntent(game_intent)
                .build();
        return ShortcutManagerCompat.requestPinShortcut(context, shortcut, null);
    }

    @Keep
    public boolean createShortcut(String game_id, String game_name){
        return createShortcut(getContext(), game_id, game_name, null);
    }

    @Keep
    public void showFileDialog() {
        if (!StorageAccess.hasStorageAccess(this)) {
            requestFolderAccess();
            return;
        }

        startActivityForResult(
                Intent.createChooser(
                        StorageAccess.createFilePickerIntent(new String[]{"*/*"}),
                        "Choose a file"),
                FILE_DIALOG_CODE);
    }

    @Keep
    public void showFolderDialog() {
        if (!StorageAccess.hasStorageAccess(this)) {
            requestFolderAccess();
            return;
        }

        startActivityForResult(
                Intent.createChooser(StorageAccess.createFolderPickerIntent(), "Choose a folder"),
                FOLDER_DIALOG_CODE);
    }

    public void requestImagePath(ImagePathResultCallback onResult) {
        pendingImagePathCallback = onResult;
        if (!StorageAccess.hasStorageAccess(this)) {
            requestFolderAccess();
            return;
        }

        launchImagePicker();
    }

    private void launchImagePicker() {
        startActivityForResult(
                Intent.createChooser(
                        StorageAccess.createFilePickerIntent(new String[]{"image/*"}),
                        "Choose an image"),
                FILE_DIALOG_CODE);
    }

    private void dispatchPendingImagePath(String path) {
        ImagePathResultCallback callback = pendingImagePathCallback;
        pendingImagePathCallback = null;
        if (callback != null) {
            callback.onResult(path);
        }
    }

    @Keep
    public int getNativeDisplayRotation() {
        // Returns the device's default display rotation (0, 90, 180, or 270 degrees)
        return getWindowManager().getDefaultDisplay().getRotation();
    }

    private void refreshNativeDisplayRotation() {
        try {
            setNativeDisplayRotation(getNativeDisplayRotation());
        } catch (Throwable ignored) {
        }
    }

    private native void setNativeDisplayRotation(int rotation);

    public native void filedialogReturn(String result_path);

    private void scheduleVitaTextEditSwap() {
        final View anchor = mSurface != null ? mSurface : getWindow().getDecorView();
        anchor.post(() -> {
            if (mTextEdit == null && nativeKeyboardRequested) {
                anchor.post(this::ensureVitaTextEdit);
                return;
            }
            ensureVitaTextEdit();
        });
    }

    private void ensureVitaTextEdit() {
        if (!(mLayout instanceof RelativeLayout) || mTextEdit == null) {
            return;
        }

        captureTextInputState(mTextEdit);
        if (mTextEdit instanceof VitaTextEdit) {
            return;
        }

        final RelativeLayout.LayoutParams params =
                lastTextInputLayoutParams != null
                        ? new RelativeLayout.LayoutParams(lastTextInputLayoutParams)
                        : new RelativeLayout.LayoutParams(mTextEdit.getLayoutParams());
        final boolean visible = mTextEdit.getVisibility() == View.VISIBLE;
        final boolean focused = mTextEdit.hasFocus();

        mLayout.removeView(mTextEdit);
        final VitaTextEdit replacement = new VitaTextEdit(this);
        replacement.setInputType(lastTextInputType);
        mTextEdit = replacement;
        mLayout.addView(mTextEdit, params);

        if (visible) {
            mTextEdit.setVisibility(View.VISIBLE);
            if (focused || nativeKeyboardRequested) {
                mTextEdit.requestFocus();
                showSoftKeyboard(mTextEdit);
            }
        }
    }

    private void captureTextInputState(SDLDummyEdit textEdit) {
        if (textEdit == null) {
            return;
        }

        if (textEdit.getLayoutParams() instanceof RelativeLayout.LayoutParams) {
            lastTextInputLayoutParams = new RelativeLayout.LayoutParams(
                    (RelativeLayout.LayoutParams) textEdit.getLayoutParams());
        }
        lastTextInputType = VitaTextEdit.getInputTypeValue(textEdit);
        hasLastTextInputState = true;
    }

    private void restoreVitaTextInput() {
        if (!hasLastTextInputState || !(mLayout instanceof RelativeLayout) || lastTextInputLayoutParams == null) {
            return;
        }

        if (mTextEdit == null) {
            final VitaTextEdit replacement = new VitaTextEdit(this);
            replacement.setInputType(lastTextInputType);
            mTextEdit = replacement;
            mLayout.addView(mTextEdit, new RelativeLayout.LayoutParams(lastTextInputLayoutParams));
        } else {
            ensureVitaTextEdit();
            mTextEdit.setLayoutParams(new RelativeLayout.LayoutParams(lastTextInputLayoutParams));
            mTextEdit.setInputType(lastTextInputType);
        }

        mTextEdit.setVisibility(View.VISIBLE);
        mTextEdit.requestFocus();
        showSoftKeyboard(mTextEdit);
    }

    private void showSoftKeyboard(View view) {
        final InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) {
            imm.showSoftInput(view, 0);
        }
        ensureOverlayUiOrder();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        switch (requestCode) {
            case FOLDER_ACCESS_SETTINGS_CODE:
                if (StorageAccess.hasStorageAccess(this)) {
                    if (pendingImagePathCallback != null) {
                        launchImagePicker();
                    } else {
                        showFolderDialog();
                    }
                } else {
                    if (pendingImagePathCallback != null) {
                        dispatchPendingImagePath(null);
                    } else {
                        filedialogReturn("");
                    }
                }
                break;
            case FILE_DIALOG_CODE:
                if (pendingImagePathCallback != null) {
                    final String resolvedPath = resultCode == RESULT_OK && data != null && data.getData() != null
                            ? StorageAccess.resolveUriToPath(this, data.getData())
                            : null;
                    dispatchPendingImagePath(resolvedPath);
                    break;
                }
                if (resultCode != RESULT_OK || data == null || data.getData() == null) {
                    filedialogReturn("");
                    break;
                }
                {
                    final String resolvedPath = StorageAccess.resolveUriToPath(this, data.getData());
                    filedialogReturn(resolvedPath != null ? resolvedPath : "");
                }
                break;
            case FOLDER_DIALOG_CODE: {
                final String selectedPath = resultCode == RESULT_OK && data != null && data.getData() != null
                        ? StorageAccess.resolveTreeUriToPath(this, data.getData())
                        : null;
                filedialogReturn(selectedPath != null ? selectedPath : "");
                break;
            }
            default:
                break;
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);

        if (requestCode != FOLDER_ACCESS_PERMISSION_CODE) {
            return;
        }

        if (StorageAccess.hasStorageAccess(this)) {
            if (pendingImagePathCallback != null) {
                launchImagePicker();
            } else {
                showFolderDialog();
            }
        } else {
            if (pendingImagePathCallback != null) {
                dispatchPendingImagePath(null);
            } else {
                filedialogReturn("");
            }
        }
    }

    private void requestFolderAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            startActivityForResult(
                    StorageAccess.createManageAllFilesIntent(this),
                    FOLDER_ACCESS_SETTINGS_CODE);
            return;
        }

        ActivityCompat.requestPermissions(
                this,
                StorageAccess.missingStoragePermissions(this),
                FOLDER_ACCESS_PERMISSION_CODE);
    }

    private void applyImmersiveMode() {
        WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
        WindowInsetsControllerCompat controller =
                ViewCompat.getWindowInsetsController(getWindow().getDecorView());
        if (controller != null) {
            controller.setSystemBarsBehavior(
                    WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            controller.hide(WindowInsetsCompat.Type.systemBars());
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            if (attributes.layoutInDisplayCutoutMode
                    != WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES) {
                attributes.layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                getWindow().setAttributes(attributes);
            }
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    private void installPauseMenu() {
        if (!(SDLActivity.getContentView() instanceof ViewGroup))
            return;

        ViewGroup contentView = (ViewGroup) SDLActivity.getContentView();
        prepareComposeHostView(contentView);
        pauseMenuView = new ComposeView(this);
        prepareComposeOverlayView(pauseMenuView);
        pauseMenuView.setLayoutParams(new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        pauseMenuView.setVisibility(View.GONE);
        contentView.addView(pauseMenuView);
        ensureOverlayUiOrder();
        EmulationPauseMenuHost.attach(
                this,
                pauseMenuView,
                sessionViewModel,
                getPauseSettingsViewModel(),
                getPauseGlobalSettingsViewModel());
    }

    private void installImeOverlay() {
        if (!(SDLActivity.getContentView() instanceof ViewGroup))
            return;

        ViewGroup contentView = (ViewGroup) SDLActivity.getContentView();
        prepareComposeHostView(contentView);
        imeOverlayView = new ComposeView(this);
        prepareComposeOverlayView(imeOverlayView);
        if (contentView instanceof RelativeLayout) {
            RelativeLayout.LayoutParams params = new RelativeLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            params.addRule(RelativeLayout.ALIGN_PARENT_TOP);
            imeOverlayView.setLayoutParams(params);
        } else {
            imeOverlayView.setLayoutParams(new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT));
        }
        imeOverlayView.setVisibility(View.GONE);
        contentView.addView(imeOverlayView);
        NativeImeOverlayHost.attach(imeOverlayView, sessionViewModel);
        ensureOverlayUiOrder();
    }

    private void installKeyboardVisibilityListener() {
        View root = getWindow().getDecorView();
        ViewCompat.setOnApplyWindowInsetsListener(root, (view, insets) -> {
            handleImeVisibilityChanged(insets.isVisible(WindowInsetsCompat.Type.ime()));
            return insets;
        });
        root.post(() -> {
            WindowInsetsCompat insets = ViewCompat.getRootWindowInsets(root);
            handleImeVisibilityChanged(insets != null && insets.isVisible(WindowInsetsCompat.Type.ime()));
        });
    }

    private void prepareComposeOverlayView(ComposeView composeView) {
        ViewTreeLifecycleOwner.set(composeView, this);
        ViewTreeSavedStateRegistryOwner.set(composeView, this);
        ViewTreeViewModelStoreOwner.set(composeView, this);
    }

    private void prepareComposeHostTree() {
        prepareComposeHostView(getWindow().getDecorView());
        View contentView = SDLActivity.getContentView();
        if (contentView != null) {
            prepareComposeHostView(contentView);
        }
    }

    private void prepareComposeHostView(View view) {
        if (view == null) {
            return;
        }
        ViewTreeLifecycleOwner.set(view, this);
        ViewTreeSavedStateRegistryOwner.set(view, this);
        ViewTreeViewModelStoreOwner.set(view, this);
    }

    private SettingsViewModel getPauseSettingsViewModel() {
        if (pauseSettingsViewModel == null) {
            ViewModelProvider provider = new ViewModelProvider(
                    this,
                    ViewModelProvider.AndroidViewModelFactory.getInstance(getApplication()));
            pauseSettingsViewModel = provider.get("pause-per-app-settings", SettingsViewModel.class);
        }
        return pauseSettingsViewModel;
    }

    private SettingsViewModel getPauseGlobalSettingsViewModel() {
        if (pauseGlobalSettingsViewModel == null) {
            ViewModelProvider provider = new ViewModelProvider(
                    this,
                    ViewModelProvider.AndroidViewModelFactory.getInstance(getApplication()));
            pauseGlobalSettingsViewModel = provider.get("pause-global-controller-settings", SettingsViewModel.class);
        }
        return pauseGlobalSettingsViewModel;
    }

    @Override
    public Lifecycle getLifecycle() {
        return lifecycleRegistry;
    }

    @Override
    public SavedStateRegistry getSavedStateRegistry() {
        return savedStateRegistryController.getSavedStateRegistry();
    }

    @Override
    public ViewModelStore getViewModelStore() {
        return composeViewModelStore;
    }

    private void handleImeVisibilityChanged(boolean visible) {
        if (imeVisible == visible) {
            return;
        }

        imeVisible = visible;
        if (visible) {
            imeWasVisibleSinceRequest = true;
            ensureOverlayUiOrder();
            return;
        }

        if (!nativeKeyboardRequested || imeDismissedByUser) {
            return;
        }

        if (suppressImeHiddenHandler) {
            suppressImeHiddenHandler = false;
            return;
        }

        if (sessionViewModel != null
                && (sessionViewModel.getUiState().getShowMenu()
                || sessionViewModel.getUiState().isEditingControls())) {
            restoreImeAfterPauseMenu = true;
            return;
        }

        if (imeWasVisibleSinceRequest) {
            dismissImeFromKeyboard(mTextEdit);
        }
    }

    private void applyKeyboardOverlayState(boolean active) {
        InputOverlay overlay = getmOverlay();
        if (overlay == null) {
            return;
        }

        if (active) {
            mSavedOverlayMask = overlay.getOverlayMask();
            setControllerOverlayState(0, false, false);
        } else {
            setControllerOverlayState(mSavedOverlayMask, false, false);
        }
    }

    private boolean ensureNativeSessionInitialized() {
        if (NativeLib.INSTANCE.isInitialized()) {
            return true;
        }

        final String storagePath = AppStorage.INSTANCE.storageRootPath(this);
        final boolean initialized = NativeLib.INSTANCE.init(storagePath);
        if (!initialized) {
            Log.e(TAG, "Failed to initialize native session for direct emulator launch");
            Toast.makeText(this, "Failed to initialize Vita3K.", Toast.LENGTH_LONG).show();
        }
        return initialized;
    }

    private void suspendForBackgroundIfNeeded() {
        if (isFinishing()) {
            return;
        }

        boolean running = false;
        try {
            running = NativeLib.INSTANCE.isAppRunning();
        } catch (Throwable ignored) {
        }
        if (!running) {
            return;
        }

        try {
            NativeLib.INSTANCE.setPauseReasonEnabled(NativeLib.PAUSE_REASON_BACKGROUND, true);
        } catch (Throwable ignored) {
        }
    }

    private void resumeFromBackgroundIfNeeded() {
        final View anchor = mSurface != null ? mSurface : getWindow().getDecorView();
        anchor.post(() -> {
            boolean running = false;
            try {
                running = NativeLib.INSTANCE.isAppRunning();
            } catch (Throwable ignored) {
            }
            if (!running) {
                return;
            }
            try {
                NativeLib.INSTANCE.setPauseReasonEnabled(NativeLib.PAUSE_REASON_BACKGROUND, false);
            } catch (Throwable ignored) {
            }
        });
    }

    private boolean hasLaunchRequest(Intent intent) {
        return intent != null
                && (hasRestartParameters(intent) || !resolveCurrentTitleId(intent).isEmpty());
    }

    private boolean hasRestartParameters(Intent intent) {
        String[] args = intent != null ? intent.getStringArrayExtra(APP_RESTART_PARAMETERS) : null;
        return args != null && args.length > 0;
    }

    private boolean isSameLaunchRequest(Intent currentIntent, Intent intent) {
        final String[] currentArgs = currentIntent != null
                ? currentIntent.getStringArrayExtra(APP_RESTART_PARAMETERS)
                : null;
        final String[] incomingArgs = intent.getStringArrayExtra(APP_RESTART_PARAMETERS);
        if (!Arrays.equals(currentArgs, incomingArgs)) {
            return false;
        }

        return resolveCurrentTitleId(currentIntent).equals(resolveCurrentTitleId(intent));
    }

    private String resolveCurrentTitleId(Intent intent) {
        if (intent == null)
            return "";

        String titleId = intent.getStringExtra(EXTRA_TITLE_ID);
        if (titleId != null && !titleId.isEmpty())
            return titleId;

        String action = intent.getAction();
        if (action != null && action.startsWith("LAUNCH_"))
            return action.substring(7);

        String[] args = intent.getStringArrayExtra(APP_RESTART_PARAMETERS);
        if (args != null) {
            for (int i = 0; i + 1 < args.length; i++) {
                if ("-r".equals(args[i]))
                    return args[i + 1];
            }
        }

        return "";
    }

    private void requestRelaunchFromIntent(Intent intent) {
        final String[] args = intent != null ? intent.getStringArrayExtra(APP_RESTART_PARAMETERS) : null;
        if (args != null && args.length > 0) {
            final String titleId = resolveCurrentTitleId(intent);
            final String selfPath = extractSelfPath(args);
            requestNativeRelaunch(titleId, selfPath, extractGuestArgs(args), !selfPath.isEmpty());
            return;
        }

        final String titleId = resolveCurrentTitleId(intent);
        if (!titleId.isEmpty()) {
            requestNativeRelaunch(titleId, "", new String[0], false);
        }
    }

    private void requestNativeRelaunch(String titleId, String selfPath, String[] args, boolean loadExecReason) {
        try {
            NativeLib.INSTANCE.requestAppRelaunch(
                    titleId != null ? titleId : "",
                    selfPath != null ? selfPath : "",
                    args != null ? args : new String[0],
                    loadExecReason);
        } catch (Throwable error) {
            Log.e(TAG, "Failed to request native relaunch", error);
        }
    }

    private String extractSelfPath(String[] args) {
        if (args == null) {
            return "";
        }

        for (int i = 0; i + 1 < args.length; i++) {
            if ("--self".equals(args[i])) {
                return args[i + 1];
            }
        }

        return "";
    }

    private String[] extractGuestArgs(String[] args) {
        if (args == null) {
            return new String[0];
        }

        for (int i = 0; i + 1 < args.length; i++) {
            if ("--app-args".equals(args[i])) {
                return new String[]{args[i + 1]};
            }
        }

        return new String[0];
    }

    private void updateControllerConnectionState() {
        final List<ConnectedGamepad> pads = InputDeviceUtils.getPhysicalGamepads();
        StringBuilder text = new StringBuilder();
        for (ConnectedGamepad pad : pads)
            text.append(text.length() == 0 ? "" : ", ").append('\'').append(pad.getName()).append("' (device ").append(pad.getDeviceId()).append(')');
        final String padList = text.length() == 0 ? "none" : text.toString();
        if (!padList.equals(lastAndroidPads)) {
            lastAndroidPads = padList;
            logInputDiagnostic("[INPUT] pads found by the app's own check (it hides the on-screen controls): " + padList);
        }
        if (sessionViewModel != null) {
            sessionViewModel.setControllerConnected(this, !pads.isEmpty());
        }
    }

    private void refreshUiState(boolean forceOverlayRebind) {
        if (isFinishing()) {
            return;
        }

        setWindowStyle(true);
        applyImmersiveMode();
        syncOverlayFromSession(forceOverlayRebind);
        ensureOverlayUiOrder();

        View decorView = getWindow().getDecorView();
        decorView.post(() -> {
            if (!isFinishing()) {
                applyImmersiveMode();
                ensureOverlayUiOrder();
            }
        });
    }

    private void syncOverlayFromSession() {
        syncOverlayFromSession(false);
    }

    private void syncOverlayFromSession(boolean forceOverlayRebind) {
        InputOverlay overlay = getmOverlay();
        if (sessionViewModel != null) {
            sessionViewModel.applyOverlayState(this);
        }
        if (nativeKeyboardRequested) {
            applyKeyboardOverlayState(true);
        }
        if (forceOverlayRebind && overlay != null) {
            overlay.rebindController();
        }
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        logInputDeviceChange("added", deviceId);
        updateControllerConnectionState();
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        logInputDeviceChange("removed", deviceId);
        dropDirectPad(deviceId, "removed");
        updateControllerConnectionState();
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        logInputDeviceChange("changed", deviceId);
        dropDirectPad(deviceId, "changed");
        updateControllerConnectionState();
    }
}
