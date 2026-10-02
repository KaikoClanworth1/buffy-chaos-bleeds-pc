// Buffy the Vampire Slayer: Chaos Bleeds - the Xbox disc's file system
// (XDVDFS), read from a disc image: a full ISO (the game partition is at
// one of a few known offsets) or an XISO (the partition alone). The PC
// launcher's reader (launcher/buffy_launcher.c, disc_open) in Java.

package io.github.kaikoclanworth1.buffy;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

final class Xdvdfs {
    static final class Entry {
        final String path;            // with '/' between folders
        final long sector, size;
        Entry(String path, long sector, long size) { this.path = path; this.sector = sector; this.size = size; }
    }

    // The releases the APK plays: the certificate's region and version and
    // the size of default.xbe's code (src/buffy_xbecheck.c).
    static final int RELEASE_NONE = 0, RELEASE_PAL = 1, RELEASE_USA = 2;

    private static final long[] kBases = { 0, 0x18300000L, 0xFD90000L, 0x2080000L };
    private final FileChannel ch_;
    private long base_;
    final List<Entry> files = new ArrayList<>();
    long total;

    Xdvdfs(FileChannel ch) { ch_ = ch; }

    private void read(long off, ByteBuffer b) throws IOException {
        b.clear();
        long pos = base_ + off;
        while (b.hasRemaining()) {
            int n = ch_.read(b, pos);
            if (n < 0) throw new IOException("the image ends early");
            pos += n;
        }
        b.flip();
    }

    // Finds the file system and lists every file; an error message, or null.
    String open() {
        ByteBuffer vd = ByteBuffer.allocate(2048).order(ByteOrder.LITTLE_ENDIAN);
        byte[] magic = "MICROSOFT*XBOX*MEDIA".getBytes();
        for (long b : kBases) {
            base_ = b;
            try {
                read(32 * 2048, vd);
            } catch (IOException e) {
                continue;
            }
            if (!startsWith(vd, 0, magic) || !startsWith(vd, 0x7EC, magic)) continue;
            long root = vd.getInt(20) & 0xFFFFFFFFL, rsize = vd.getInt(24) & 0xFFFFFFFFL;
            try {
                dir(root, rsize, "", 0);
            } catch (IOException e) {
                return "The disc image is damaged: its file list could not be read.";
            }
            return null;
        }
        return "This is not an Xbox disc image. (A PlayStation 2 or GameCube image of the game will not work: "
            + "the port needs the Xbox version.)";
    }

    private static boolean startsWith(ByteBuffer b, int at, byte[] m) {
        for (int i = 0; i < m.length; i++) if (b.get(at + i) != m[i]) return false;
        return true;
    }

    private void dir(long sector, long size, String prefix, int depth) throws IOException {
        if (depth > 32 || size > (16 << 20)) throw new IOException("bad folder");
        ByteBuffer d = ByteBuffer.allocate((int) size).order(ByteOrder.LITTLE_ENDIAN);
        read(sector * 2048, d);
        node(d, 0, prefix, depth, 0);
    }

    // One node of a folder's binary tree (offset in dwords), in order.
    private void node(ByteBuffer d, int off, String prefix, int depth, int guard) throws IOException {
        int at = off * 4;
        if (guard > 4096 || at + 14 > d.limit()) return;
        int left = d.getShort(at) & 0xFFFF, right = d.getShort(at + 2) & 0xFFFF;
        if (left == 0xFFFF && right == 0xFFFF) return;          // padding
        long sec = d.getInt(at + 4) & 0xFFFFFFFFL, sz = d.getInt(at + 8) & 0xFFFFFFFFL;
        int attr = d.get(at + 12) & 0xFF, nlen = d.get(at + 13) & 0xFF;
        if (at + 14 + nlen > d.limit()) throw new IOException("bad entry");
        StringBuilder name = new StringBuilder();
        for (int i = 0; i < nlen; i++) name.append((char) (d.get(at + 14 + i) & 0xFF));
        String n = name.toString();
        if (n.isEmpty() || n.contains("/") || n.contains("\\") || n.equals("..") || n.equals("."))
            throw new IOException("unsafe name");
        if (left != 0) node(d, left, prefix, depth, guard + 1);
        String path = prefix.isEmpty() ? n : prefix + "/" + n;
        if ((attr & 0x10) != 0) {
            if (sz != 0) dir(sec, sz, path, depth + 1);
        } else {
            files.add(new Entry(path, sec, sz));
            total += sz;
        }
        if (right != 0) node(d, right, prefix, depth, guard + 1);
    }

    Entry find(String path) {
        for (Entry e : files) if (e.path.equalsIgnoreCase(path)) return e;
        return null;
    }

    // Which release default.xbe is (RELEASE_*).
    int release() {
        Entry x = find("default.xbe");
        if (x == null || x.size < 0x1000) return RELEASE_NONE;
        try {
            ByteBuffer h = ByteBuffer.allocate(0x1000).order(ByteOrder.LITTLE_ENDIAN);
            read(x.sector * 2048, h);
            if (h.get(0) != 'X' || h.get(1) != 'B' || h.get(2) != 'E' || h.get(3) != 'H') return RELEASE_NONE;
            long base = h.getInt(0x104) & 0xFFFFFFFFL;
            int cert = (int) ((h.getInt(0x118) & 0xFFFFFFFFL) - base), sect = (int) ((h.getInt(0x120) & 0xFFFFFFFFL) - base);
            if (cert < 0 || sect < 0 || cert + 0xB0 > 0x1000 || sect + 12 > 0x1000) return RELEASE_NONE;
            int region = h.getInt(cert + 0xA0), version = h.getInt(cert + 0xAC), text = h.getInt(sect + 8);
            if (region == 0x4 && version == 2 && text == 1196336) return RELEASE_PAL;
            if (region == 0x3 && version == 1 && text == 1193024) return RELEASE_USA;
        } catch (IOException ignored) {
        }
        return RELEASE_NONE;
    }

    interface Progress { void at(long done, long total, String file); }

    // Every file into dest (default.xbe last: an interrupted install is not
    // taken for a whole one).
    void extract(File dest, Progress p, AtomicBoolean cancel) throws IOException {
        long done = 0;
        ByteBuffer buf = ByteBuffer.allocateDirect(4 << 20);
        String root = dest.getCanonicalPath() + File.separator;
        List<Entry> order = new ArrayList<>(files);
        Entry xbe = find("default.xbe");
        order.remove(xbe);
        order.add(xbe);
        for (Entry e : order) {
            if (cancel.get()) throw new IOException("cancelled");
            File out = new File(dest, e.path);
            if (!out.getCanonicalPath().startsWith(root)) throw new IOException("bad path: " + e.path);
            File parent = out.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs())
                throw new IOException("could not make " + parent);
            try (FileOutputStream os = new FileOutputStream(out); FileChannel oc = os.getChannel()) {
                long left = e.size, pos = base_ + e.sector * 2048;
                while (left > 0) {
                    if (cancel.get()) throw new IOException("cancelled");
                    buf.clear();
                    if (left < buf.capacity()) buf.limit((int) left);
                    int n = ch_.read(buf, pos);
                    if (n <= 0) throw new IOException("the image ends early");
                    buf.flip();
                    while (buf.hasRemaining()) oc.write(buf);
                    pos += n;
                    left -= n;
                    done += n;
                    p.at(done, total, e.path);
                }
            }
        }
    }
}
