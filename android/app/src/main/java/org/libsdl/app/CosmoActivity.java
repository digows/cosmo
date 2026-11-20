package org.libsdl.app;

import android.os.Bundle;
import android.view.KeyEvent;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/** Prepares bundled game data and mods before SDL starts the native entry point. */
public final class CosmoActivity extends SDLActivity {
    private void sendEscapeToSDL() {
        SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_ESCAPE);
        SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_ESCAPE);
    }

    @Override
    public void onBackPressed() {
        sendEscapeToSDL();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()) {
                sendEscapeToSDL();
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        try {
            File files = getFilesDir();
            copyAssetFileIfMissing("COSMO1.STN", new File(files, "COSMO1.STN"));
            copyAssetFileIfMissing("COSMO1.VOL", new File(files, "COSMO1.VOL"));
            copyAssetTree("Mods", new File(files, "Mods"));
        } catch (IOException e) {
            throw new IllegalStateException("Could not prepare bundled Cosmo data", e);
        }

        super.onCreate(savedInstanceState);
    }

    private void copyAssetTree(String assetPath, File destination) throws IOException {
        String[] children = getAssets().list(assetPath);
        if (children == null || children.length == 0) {
            copyAssetFile(assetPath, destination, true);
            return;
        }

        if (!destination.isDirectory() && !destination.mkdirs()) {
            throw new IOException("Could not create " + destination);
        }

        for (String child : children) {
            copyAssetTree(assetPath + "/" + child, new File(destination, child));
        }
    }

    private void copyAssetFileIfMissing(String assetPath, File destination) throws IOException {
        if (!destination.isFile() || destination.length() == 0) {
            copyAssetFile(assetPath, destination, true);
        }
    }

    private void copyAssetFile(String assetPath, File destination, boolean replace)
            throws IOException {
        if (destination.isFile() && !replace) {
            return;
        }

        File parent = destination.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("Could not create " + parent);
        }

        try (InputStream input = getAssets().open(assetPath);
             FileOutputStream output = new FileOutputStream(destination)) {
            byte[] buffer = new byte[16 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                output.write(buffer, 0, count);
            }
        }
    }
}
