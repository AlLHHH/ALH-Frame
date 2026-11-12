package com.alh.frame;

import android.content.Context;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;

/**
 * 内置 ffmpeg 的落地与版本管理。
 */
public final class Ff {

    /** 内置 ffmpeg 的版本标记，换 ffmpeg 二进制时必须同步改它。 */
    static final String VER = "9.0.2-alhframe-1";

    static String bin;

    private Ff() {
    }

    public static synchronized String bin(Context ctx) throws Exception {
        if (bin != null && new File(bin).canExecute()) {
            return bin;
        }
        File dir = new File(ctx.getFilesDir(), "bin");
        if (!dir.isDirectory() && !dir.mkdirs()) {
            throw new Exception("无法创建 bin 目录");
        }
        File file = new File(dir, "ffmpeg");
        File verFile = new File(dir, ".ver");
        if (!file.exists() || !VER.equals(readAll(verFile))) {
            InputStream in = ctx.getAssets().open("ff/ffmpeg");
            FileOutputStream out = new FileOutputStream(file);
            byte[] buf = new byte[1048576];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            in.close();
            out.close();
            file.setExecutable(true, false);
            writeAll(verFile, VER);
        }
        bin = file.getAbsolutePath();
        return bin;
    }

    static String readAll(File f) {
        String s = "";
        try {
            if (!f.exists()) {
                return s;
            }
            byte[] buf = new byte[(int) f.length()];
            FileInputStream in = new FileInputStream(f);
            int n = in.read(buf);
            in.close();
            s = new String(buf, 0, Math.max(n, 0), "UTF-8");
            return s.trim();
        } catch (Throwable ignored) {
            return s;
        }
    }

    static void writeAll(File f, String s) {
        try {
            FileOutputStream out = new FileOutputStream(f);
            out.write(s.getBytes("UTF-8"));
            out.close();
        } catch (Throwable ignored) {
        }
    }
}