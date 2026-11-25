package com.chromevoid.nest;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.View;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/* Nest on Android: SDL runs Nest's own code (libmain.so). This class only asks for
   access to the user's files and keeps music going when Nest is in the background. */
public class NestActivity extends SDLActivity {
    static native boolean nativeAudioBusy();

    private boolean askedForFiles;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 28) {
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        askForFiles();
    }

    /* Nest reads videos, music, books and subtitles from anywhere the user keeps them */
    private void askForFiles() {
        if (askedForFiles) return;
        askedForFiles = true;
        if (Build.VERSION.SDK_INT >= 30) {
            if (!Environment.isExternalStorageManager()) {
                try {
                    startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
                } catch (Exception e) {
                    try {
                        startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                    } catch (Exception ignored) {
                    }
                }
            }
            if (Build.VERSION.SDK_INT >= 33) {
                requestPermissions(new String[] { "android.permission.POST_NOTIFICATIONS" }, 2);
            }
        } else {
            requestPermissions(new String[] {
                "android.permission.READ_EXTERNAL_STORAGE",
                "android.permission.WRITE_EXTERNAL_STORAGE" }, 1);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    static boolean audioBusy() {
        try {
            return nativeAudioBusy();
        } catch (Throwable t) {
            return false;
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        /* music playing: a foreground service keeps Nest alive with the screen off */
        if (audioBusy()) {
            try {
                startForegroundService(new Intent(this, NestPlaybackService.class));
            } catch (Throwable ignored) {
            }
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        try {
            stopService(new Intent(this, NestPlaybackService.class));
        } catch (Throwable ignored) {
        }
    }
}
