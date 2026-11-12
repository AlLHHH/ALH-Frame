package com.alh.frame;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * 滚动日志：单个文件，上限 5 MB，超出后自动裁剪到最新 3 MB。
 * 写入失败一律吞掉——日志绝不能影响补帧本身。
 */
public final class Logs {

    public static final long MAX_BYTES = 5L * 1024 * 1024;
    static final long KEEP_BYTES = 3L * 1024 * 1024;

    private static final Object LOCK = new Object();
    private static final SimpleDateFormat TS =
            new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US);

    private Logs() {
    }

    public static File file(Context c) {
        return new File(c.getFilesDir(), "alhframe.log");
    }

    public static long size(Context c) {
        try {
            return file(c).length();
        } catch (Throwable t) {
            return 0;
        }
    }

    public static void append(Context c, String tag, String msg) {
        if (msg == null) {
            return;
        }
        String body = msg.length() > 8000 ? msg.substring(0, 8000) + "…" : msg;
        synchronized (LOCK) {
            File f = file(c);
            try {
                if (f.length() > MAX_BYTES) {
                    rotate(f);
                }
                FileOutputStream out = new FileOutputStream(f, true);
                try {
                    String line = TS.format(new Date()) + " [" + tag + "] " + body + "\n";
                    out.write(line.getBytes("UTF-8"));
                    out.flush();
                } finally {
                    out.close();
                }
            } catch (Throwable ignored) {
            }
        }
    }

    /** 只保留文件尾部 KEEP_BYTES，避免整份读进内存。 */
    private static void rotate(File f) {
        try {
            long len = f.length();
            long start = Math.max(0, len - KEEP_BYTES);
            byte[] buf = new byte[(int) (len - start)];
            RandomAccessFile raf = new RandomAccessFile(f, "r");
            try {
                raf.seek(start);
                raf.readFully(buf);
            } finally {
                raf.close();
            }
            int off = 0;
            while (off < buf.length && buf[off] != '\n') {
                off++;
            }
            off = Math.min(off + 1, buf.length);
            File tmp = new File(f.getParentFile(), "alhframe.log.tmp");
            FileOutputStream out = new FileOutputStream(tmp, false);
            try {
                out.write(("---- 日志超过 " + (MAX_BYTES / 1048576) + " MB，已裁剪 ----\n")
                        .getBytes("UTF-8"));
                out.write(buf, off, buf.length - off);
                out.flush();
            } finally {
                out.close();
            }
            if (!tmp.renameTo(f)) {
                f.delete();
                tmp.renameTo(f);
            }
        } catch (Throwable ignored) {
        }
    }

    /** 读取日志尾部（最多 maxBytes 字节），返回可复制的文本。 */
    public static String tail(Context c, int maxBytes) {
        synchronized (LOCK) {
            File f = file(c);
            try {
                long len = f.length();
                if (len <= 0) {
                    return "";
                }
                int take = (int) Math.min(len, maxBytes);
                byte[] buf = new byte[take];
                RandomAccessFile raf = new RandomAccessFile(f, "r");
                try {
                    raf.seek(len - take);
                    raf.readFully(buf);
                } finally {
                    raf.close();
                }
                String s = new String(buf, "UTF-8");
                if (take < len) {
                    int nl = s.indexOf('\n');
                    if (nl >= 0 && nl + 1 < s.length()) {
                        s = s.substring(nl + 1);
                    }
                }
                return s;
            } catch (Throwable t) {
                return "";
            }
        }
    }

    public static void clear(Context c) {
        synchronized (LOCK) {
            try {
                File f = file(c);
                if (f.exists()) {
                    f.delete();
                }
                File tmp = new File(f.getParentFile(), "alhframe.log.tmp");
                if (tmp.exists()) {
                    tmp.delete();
                }
            } catch (Throwable ignored) {
            }
        }
    }
}