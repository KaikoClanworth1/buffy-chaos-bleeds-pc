// Buffy the Vampire Slayer: Chaos Bleeds - the app's cache files, read only,
// for other apps the player sends them to (the Saves tab's share sheet):
// content://io.github.kaikoclanworth1.buffy.files/cache/<name>.

package io.github.kaikoclanworth1.buffy;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

public class FilesProvider extends ContentProvider {
    private File file(Uri uri) throws FileNotFoundException {
        if (uri.getPathSegments().size() != 2 || !"cache".equals(uri.getPathSegments().get(0)))
            throw new FileNotFoundException(uri.toString());
        File dir = getContext().getCacheDir(), f = new File(dir, uri.getPathSegments().get(1));
        try {
            if (!f.getCanonicalPath().startsWith(dir.getCanonicalPath() + File.separator) || !f.isFile())
                throw new FileNotFoundException(uri.toString());
        } catch (IOException e) {
            throw new FileNotFoundException(uri.toString());
        }
        return f;
    }

    @Override public boolean onCreate() { return true; }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sort) {
        try {
            File f = file(uri);
            MatrixCursor c = new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
            c.addRow(new Object[] {f.getName(), f.length()});
            return c;
        } catch (FileNotFoundException e) {
            return null;
        }
    }

    @Override
    public String getType(Uri uri) {
        return uri.getLastPathSegment() != null && uri.getLastPathSegment().endsWith(".zip") ? "application/zip"
                                                                                             : "application/octet-stream";
    }

    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String selection, String[] args) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }
}
