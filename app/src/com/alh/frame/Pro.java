package com.alh.frame;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.ByteArrayOutputStream;

/**
 * 自研补帧内核（ALH Frame Standard）的落地与调用。
 *
 * 内核是单文件 C++，编成静态二进制随 APK 分发，按 CPU 架构选择：
 *   arm64-v8a → assets/alh/alhift-arm64-v8a
 *   x86_64    → assets/alh/alhift-x86_64（模拟器 / 少数平板）
 * 只做两帧之间生成 mult-1 张中间帧，解码编码仍交给 ffmpeg，通过管道串联。
 */
final class Pro {

    // 内核二进制换了就必须改这里，否则老装机上会继续用 files/bin 里的旧内核。
    static final String VER = "alhift-16";

    private static String cached;

    private Pro() {
    }

    static String abi() {
        String[] a = android.os.Build.SUPPORTED_ABIS;
        if (a == null || a.length == 0) {
            return null;
        }
        String s = a[0];
        if (s.contains("arm64")) {
            return "arm64-v8a";
        }
        if (s.contains("x86_64")) {
            return "x86_64";
        }
        return null;
    }

    /** 返回可执行路径；当前架构没有对应内核时返回 null，调用方回落到 Lite 路径。 */
    static synchronized String bin(Context ctx) {
        if (cached != null) {
            File f = new File(cached);
            if (f.exists() && f.canExecute()) {
                return cached;
            }
            cached = null;
        }
        String abi = abi();
        if (abi == null) {
            return null;
        }
        File dir = new File(ctx.getFilesDir(), "bin");
        if (!dir.exists() && !dir.mkdirs()) {
            return null;
        }
        File exe = new File(dir, "alhift");
        File ver = new File(dir, ".ver_pro");
        if (!exe.exists() || !VER.equals(read(ver))) {
            try {
                InputStream in = ctx.getAssets().open("alh/alhift-" + abi);
                FileOutputStream out = new FileOutputStream(exe);
                byte[] buf = new byte[262144];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
                out.flush();
                out.close();
                in.close();
                exe.setExecutable(true, false);
                FileOutputStream v = new FileOutputStream(ver);
                v.write(VER.getBytes("UTF-8"));
                v.close();
            } catch (Throwable t) {
                android.util.Log.w("ALH", "释放内核失败：" + t);
                return null;
            }
        }
        if (!exe.canExecute()) {
            exe.setExecutable(true, false);
        }
        cached = exe.getAbsolutePath();
        return cached;
    }

    private static String read(File f) {
        try {
            InputStream in = new java.io.FileInputStream(f);
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] b = new byte[256];
            int n;
            while ((n = in.read(b)) > 0) {
                bo.write(b, 0, n);
            }
            in.close();
            return new String(bo.toByteArray(), "UTF-8").trim();
        } catch (Throwable t) {
            return null;
        }
    }
}