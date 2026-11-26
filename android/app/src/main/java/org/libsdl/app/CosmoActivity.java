package org.libsdl.app;

import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.KeyEvent;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;

/** Prepares bundled game data and mods before SDL starts the native entry point. */
public final class CosmoActivity extends SDLActivity {
    private static final int EPISODE_PICKER_REQUEST = 132;
    private static final long ESCAPE_HOLD_MS = 120;
    private volatile int episodeImportResult;
    private int importEpisode;
    private final Handler keyHandler = new Handler(Looper.getMainLooper());
    private final Runnable releaseEscape = () -> SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_ESCAPE);

    private void sendEscapeToSDL() {
        keyHandler.removeCallbacks(releaseEscape);
        SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_ESCAPE);
        // Cosmo polls the held key state once per game tick.
        keyHandler.postDelayed(releaseEscape, ESCAPE_HOLD_MS);
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

    // Called by the native launcher. The launcher polls until the import finishes.
    public void startEpisodeImport(int episode) {
        episodeImportResult = 0;
        runOnUiThread(() -> {
            importEpisode = episode;
            new AlertDialog.Builder(this)
                    .setTitle("Import Episode " + episode)
                    .setMessage("Select COSMO" + episode + ".STN and COSMO" + episode
                            + ".VOL together. Touch and hold one file to start selecting.")
                    .setNegativeButton("Cancel", (dialog, which) -> episodeImportResult = -1)
                    .setOnCancelListener(dialog -> episodeImportResult = -1)
                    .setPositiveButton("Choose files", (dialog, which) -> openEpisodePicker())
                    .show();
        });
    }

    public int getEpisodeImportResult() {
        return episodeImportResult;
    }

    private void openEpisodePicker() {
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType("*/*");
        picker.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        try {
            startActivityForResult(picker, EPISODE_PICKER_REQUEST);
        } catch (ActivityNotFoundException e) {
            reportImportError("No file picker is available on this device.");
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != EPISODE_PICKER_REQUEST) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode != RESULT_OK || data == null) {
            episodeImportResult = -1;
            return;
        }

        ClipData files = data.getClipData();
        if (files == null || files.getItemCount() != 2) {
            reportImportError("Select both the STN and VOL files together.");
            return;
        }

        int episode = importEpisode;
        Uri first = files.getItemAt(0).getUri();
        Uri second = files.getItemAt(1).getUri();
        new Thread(() -> importEpisodeFiles(episode, first, second), "Cosmo episode import").start();
    }

    private void reportImportError(String message) {
        runOnUiThread(() -> new AlertDialog.Builder(this)
                .setTitle("Episode import failed")
                .setMessage(message)
                .setPositiveButton("Back", (dialog, which) -> episodeImportResult = -1)
                .setOnCancelListener(dialog -> episodeImportResult = -1)
                .show());
    }

    private String documentName(Uri uri) {
        if (uri == null) return "";
        try (Cursor cursor = getContentResolver().query(uri,
                new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            return cursor != null && cursor.moveToFirst() ? cursor.getString(0) : "";
        }
    }

    private void importEpisodeFiles(int episode, Uri first, Uri second) {
        String stem = "COSMO" + episode;
        String stnName = stem + ".STN";
        String volName = stem + ".VOL";
        File stn = new File(getFilesDir(), stnName);
        File vol = new File(getFilesDir(), volName);
        File stagedStn = null;
        File stagedVol = null;
        try {
            String firstName = documentName(first);
            String secondName = documentName(second);
            boolean stnFirst = stnName.equalsIgnoreCase(firstName) && volName.equalsIgnoreCase(secondName);
            boolean volFirst = volName.equalsIgnoreCase(firstName) && stnName.equalsIgnoreCase(secondName);
            if (!stnFirst && !volFirst) {
                reportImportError("Choose the matching " + stnName + " and " + volName + " files.");
                return;
            }
            stagedStn = File.createTempFile(stem, ".stn-import", getFilesDir());
            stagedVol = File.createTempFile(stem, ".vol-import", getFilesDir());
            copyEpisodeFile(stnFirst ? first : second, stagedStn);
            copyEpisodeFile(stnFirst ? second : first, stagedVol);
            if (!stagedStn.renameTo(stn)) throw new IOException("Could not install " + stnName);
            if (!stagedVol.renameTo(vol)) {
                stn.delete();
                throw new IOException("Could not install " + volName);
            }
            episodeImportResult = 1;
        } catch (IOException | RuntimeException e) {
            android.util.Log.w("Cosmo", "Episode import failed", e);
            reportImportError("Check that both files are readable Cosmo game data, then try again.");
        } finally {
            if (stagedStn != null) stagedStn.delete();
            if (stagedVol != null) stagedVol.delete();
        }
    }

    private void copyEpisodeFile(Uri uri, File destination) throws IOException {
        try (InputStream input = getContentResolver().openInputStream(uri);
             FileOutputStream output = new FileOutputStream(destination)) {
            if (input == null) throw new IOException("Could not open document");
            copyStream(input, output);
        }
        // A Cosmo group starts with 48 records containing a name, offset and size.
        try (RandomAccessFile group = new RandomAccessFile(destination, "r")) {
            long length = group.length();
            if (length <= 960 || group.readUnsignedByte() == 0)
                throw new IOException("Missing group header");
            for (int entry = 0; entry < 960; entry += 20) {
                group.seek(entry);
                if (group.readUnsignedByte() == 0) break;
                group.seek(entry + 12);
                long offset = Integer.reverseBytes(group.readInt()) & 0xffffffffL;
                long size = Integer.reverseBytes(group.readInt()) & 0xffffffffL;
                // Original archives end the directory with a count record.
                if (entry != 0 && offset == 0 && size == 0) break;
                if (offset < 960 || offset + size > length)
                    throw new IOException("Invalid group entry");
            }
        }
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
            copyStream(input, output);
        }
    }

    private static void copyStream(InputStream input, FileOutputStream output) throws IOException {
        byte[] buffer = new byte[16 * 1024];
        int count;
        while ((count = input.read(buffer)) != -1) {
            output.write(buffer, 0, count);
        }
    }
}
