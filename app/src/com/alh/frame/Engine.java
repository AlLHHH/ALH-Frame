package com.alh.frame;

import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaMetadataRetriever;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Build;
import android.provider.MediaStore;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/**
 * SCB-MCI 补帧引擎：minterpolate + 帧精确分段并行 + 音轨回封 + 相册落盘。
 */
public class Engine {

    /**
     * minterpolate 的固定尾巴。
     * me_mode 用 bilat（双侧匹配）而不是 bidir：实测同画质下快 1.5–1.7 倍，
     * 4x 档位下画质反而更高（大位移时 bidir 的平均更早失效）。
     * 其余参数维持标定值：aobmc 重叠块补偿、epzs 搜索、16px 块、场景切换自检。
     */
    /** 临时目录的清理与创建必须串行，否则启动时的后台清理会和开工清理互相踩。 */
    static final Object CACHE_LOCK = new Object();

    static final String MCI_TAIL =
            ":mi_mode=mci:mc_mode=aobmc:me_mode=bilat:me=epzs:search_param=32"
            + ":mb_size=16:vsbmc=1:scd=fdiff:scd_threshold=8";

    /**
     * 降级用的替换参数。minterpolate 在小尺寸、非 16 对齐的源上会让某些机型崩掉，
     * 换一条代码路径比拼参数更有用：obmc/bidir 和 aobmc/bilat 走的是不同的预测分支。
     */
    static String tailFor(int attempt) {
        if (attempt == 3) {
            return ":mi_mode=mci:mc_mode=obmc:me_mode=bidir:me=epzs:search_param=32"
                    + ":mb_size=16:vsbmc=0:scd=fdiff:scd_threshold=8";
        }
        if (attempt == 4) {
            return ":mi_mode=mci:mc_mode=obmc:me_mode=bilat:me=epzs:search_param=16"
                    + ":mb_size=8:vsbmc=0:scd=fdiff:scd_threshold=8";
        }
        if (attempt == 5) {
            return ":mi_mode=blend";
        }
        return MCI_TAIL;
    }

    /** 每档降级做了什么，写进日志用。 */
    static String stageName(int attempt) {
        switch (attempt) {
            case 1:
                return "兼容模式 1";
            case 2:
                return "兼容模式 2";
            case 3:
                return "兼容模式 3";
            case 4:
                return "兼容模式 4";
            case 5:
                return "兼容模式 5";
            case 6:
                return "兼容模式 6";
            default:
                return "正常模式";
        }
    }

    public interface Cb {
        void progress(String phase, int done, int total, long etaMs);
        void done(String path, int frames, long ms);

        /**
         * @param msg    一句话结论，UI 上直接显示
         * @param detail 完整诊断（设备、参数、每条 ffmpeg 命令、stderr 尾部），供复制
         */
        void fail(String msg, String detail);
    }

    public static class Info {
        public double fps;
        public double durSec;
        public int frames;
        public int w;
        public int h;
        public boolean hasAudio;
    }

    static class Mux {
        int rc;
        StringBuilder err = new StringBuilder();
    }

    volatile boolean cancel;
    final Cb cb;
    final Context ctx;
    final AtomicInteger doneFrames = new AtomicInteger();
    final List<Process> procs = Collections.synchronizedList(new ArrayList<Process>());
    int totalOut;

    /** 本次任务的完整轨迹，失败时整段给用户复制。 */
    final StringBuilder trace = new StringBuilder();

    /** 引擎：0 = ALH Frame Lite（ffmpeg minterpolate），1 = ALH Frame Standard（自研块匹配），2 = ALH Frame Pro（低分辨率稠密流场）。 */
    // 档位（profile）：0 Lite 最快 / 1 Standard 均衡 / 2 Pro 最慢但最好
    static final int PROFILE_LITE = 0;
    static final int PROFILE_STD = 1;
    static final int PROFILE_PRO = 2;

    int profile = PROFILE_LITE;
    /** 去重复帧：前后帧几乎一致时直接复制，不插值（Pro 内核支持）。 */
    boolean dedup;
    /** Pro 内核可执行文件路径；为空表示当前架构没有内核，自动回落 Lite。 */
    String proBin;

    public Engine(Context c, Cb cb) {
        this.ctx = c.getApplicationContext();
        this.cb = cb;
    }

    void note(String tag, String s) {
        if (s == null) {
            return;
        }
        Logs.append(ctx, tag, s);
        synchronized (trace) {
            if (trace.length() < 200 * 1024) {
                trace.append('[').append(tag).append("] ").append(s).append('\n');
            }
        }
    }

    String deviceLine() {
        String abi = Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?";
        return "ALH Frame " + appVersion()
                + " · " + Build.MANUFACTURER + " " + Build.MODEL
                + " · Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")"
                + " · " + abi
                + " · " + Runtime.getRuntime().availableProcessors() + " 核"
                + " · 可用内存 " + memAvailableMb() + " MB";
    }

    String appVersion() {
        try {
            return ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).versionName;
        } catch (Throwable t) {
            return "?";
        }
    }

    String detail(String msg) {
        StringBuilder sb = new StringBuilder();
        sb.append(deviceLine()).append('\n');
        sb.append("结论: ").append(msg).append('\n');
        sb.append("---- 执行轨迹 ----\n");
        synchronized (trace) {
            sb.append(trace);
        }
        if (trace.length() >= 200 * 1024) {
            sb.append("…（轨迹过长已截断，完整内容见日志）\n");
        }
        return sb.toString();
    }

    // ------------------------------------------------------------------ 探测

    public static Info probe(File f) throws Exception {
        Info info = new Info();
        MediaMetadataRetriever r = new MediaMetadataRetriever();
        try {
            r.setDataSource(f.getAbsolutePath());
            long ms = parseLong(r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION), 0);
            info.durSec = ms / 1000.0;
            info.hasAudio = "yes".equals(
                    r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_HAS_AUDIO));
            info.w = (int) parseLong(
                    r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH), 0);
            info.h = (int) parseLong(
                    r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT), 0);
            long rot = parseLong(
                    r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION), 0);
            if (rot == 90 || rot == 270) {
                int t = info.w;
                info.w = info.h;
                info.h = t;
            }
        } finally {
            r.release();
        }

        // 元数据里的 frame-rate 是整数，29.97 会被读成 29 或 30；VFR 源（录屏）更是完全对不上，
        // 于是按「帧数 / 时间跨度」把帧率和时长都算回来，后面切段与计时全部用这个真实值。
        double fps = 0;
        boolean complete = false;
        int counted = 0;
        long minPts = Long.MAX_VALUE;
        long maxPts = 0;
        MediaExtractor ex = new MediaExtractor();
        try {
            ex.setDataSource(f.getAbsolutePath());
            for (int t = 0; t < ex.getTrackCount(); t++) {
                MediaFormat mf = ex.getTrackFormat(t);
                String mime = mf.getString("mime");
                if (mime == null || !mime.startsWith("video/")) {
                    continue;
                }
                if (mf.containsKey("frame-rate")) {
                    try {
                        fps = mf.getInteger("frame-rate");
                    } catch (Throwable ignored) {
                        fps = 0;
                    }
                }
                ex.selectTrack(t);
                long probeStart = System.currentTimeMillis();
                complete = true;
                while (ex.advance()) {
                    if ((counted & 0xFF) == 0
                            && System.currentTimeMillis() - probeStart > 1200) {
                        complete = false;
                        break;
                    }
                    long pts = ex.getSampleTime();
                    if (pts < 0) {
                        continue;
                    }
                    counted++;
                    if (pts < minPts) {
                        minPts = pts;
                    }
                    if (pts > maxPts) {
                        maxPts = pts;
                    }
                    if (counted > 300000) {
                        complete = false;
                        break;
                    }
                }
                break;
            }
        } finally {
            ex.release();
        }
        // 只有把整条轨道数完，帧数/时长才可信；中途超时退出就退回容器元数据。
        if (complete && counted >= 2 && maxPts > minPts) {
            double span = (maxPts - minPts) / 1000000.0;
            double avg = (counted - 1) / span;
            if (avg >= 1.0 && avg <= 480.0) {
                fps = avg;
                info.durSec = span + 1.0 / avg;
                info.frames = counted;
            }
        }
        if (fps < 1.0 || fps > 480.0) {
            fps = 30.0;
        }
        info.fps = fps;
        if (info.frames <= 0) {
            info.frames = (int) Math.round(fps * info.durSec);
        }
        if (info.durSec <= 0.02 || info.frames < 2) {
            throw new Exception("源文件过短，至少需要 2 帧");
        }
        return info;
    }

    // ------------------------------------------------------------------ 主流程

    public void start(final File src, final int mult, final File outDir) {
        start(src, mult, outDir, PROFILE_LITE, false);
    }

    public void start(final File src, final int mult, final File outDir,
                      final int engineMode, final boolean dedupOn) {
        this.profile = engineMode;
        this.dedup = dedupOn;
        // 三档都走自研内核，靠搜索半径 / 匹配窗 / 亚像素精修拉开质量与耗时。
        // 内核释放失败（架构不支持）时统一回落到 ffmpeg 路径，保证还能出片。
        this.proBin = Pro.bin(ctx);
        if (proBin == null) {
            note("pro", "当前架构没有自研内核，回落 ffmpeg 路径");
        }
        new Thread(new Runnable() {
            public void run() {
                try {
                    process(src, mult, outDir);
                } catch (Throwable t) {
                    if (cancel) {
                        note("job", "用户取消");
                        // 取消后把本次的分段与累加流删掉：不删的话它们会一直占着缓存分区直到下次开工。
                        wipeTemp(ctx);
                        cb.fail("已取消", detail("已取消"));
                        return;
                    }
                    String msg = shortErr(t);
                    note("error", msg);
                    if (t instanceof Exception && t.getCause() != null) {
                        note("error", "cause: " + shortErr(t.getCause()));
                    }
                    saveLastError(msg);
                    cb.fail(plain(msg), detail(msg));
                }
            }
        }, "alhframe").start();
    }

    public void cancel() {
        cancel = true;
        synchronized (procs) {
            for (Process p : procs) {
                try {
                    p.destroy();
                } catch (Throwable ignored) {
                }
            }
        }
    }

    void process(File src, int mult, File outDir) throws Exception {
        long t0 = System.currentTimeMillis();
        cb.progress("正在准备 · 读取源文件", -1, 0, -1);
        Info ii = probe(src);

        double inFps = ii.fps;
        double dur = ii.durSec;
        double outFps = inFps * mult;
        totalOut = Math.max(1, (int) Math.round(dur * outFps));

        // 并发：minterpolate 单线程，多开进程是唯一并行手段；真正的闸门是内存。
        int cores = Runtime.getRuntime().availableProcessors();
        int cap = Math.min(8, Math.max(1, cores - 1));
        if (mult >= 4) {
            cap = Math.min(cap, 6);
        }
        cap = Math.min(cap, Math.max(1, totalOut / 24));

        // 每路峰值内存（MB）按实测校过：1080×1620 单路实测 762 MB，
        // 其中约 200 MB 是固定开销，余下跟像素量走。乘 0.6 把余量留给系统。
        double mp = (double) ii.w * ii.h / 1000000.0;
        double perWorkerMb = Math.max(360, 200 + 220 * mp);
        long mem = memAvailableMb();
        int workers = cap;
        if (mem > 0) {
            int fit = (int) Math.max(1, (long) (mem * 0.6 / perWorkerMb));
            workers = Math.min(cap, fit);
        }
        workers = Math.max(1, workers);

        // 段数跟并发解耦：每段约 30 秒。段一做完就并进累加文件并删掉，
        // 所以长视频的磁盘峰值是「成品 + 在跑的几段」，不是「整片 ×2」。
        // 每段 20 秒：段越短，某一段崩掉后重跑的代价越小，进度也跳得越细。
        int segments = (int) Math.max(workers, Math.min(240, Math.ceil(dur / 20.0)));
        segments = Math.min(segments, Math.max(1, totalOut / 24));
        workers = Math.min(workers, Math.max(1, totalOut));
        segments = Math.min(Math.max(segments, workers), Math.max(1, totalOut));

        cb.progress("正在准备 · 清理上次的临时文件", -1, totalOut, -1);
        File tempDir = new File(ctx.getCacheDir(), "alhframe");
        synchronized (CACHE_LOCK) {
            delTree(tempDir);
            if (!tempDir.mkdirs() && !tempDir.isDirectory()) {
                throw new Exception("无法创建临时目录");
            }
        }

        // 峰值估算：累加流 acc.ts(≈成品) + 成品 out.mp4(≈成品) + 源副本 + 余量。
        // 以前只算 1.25 份成品，最后一步封装写成品时会撑爆，跑到收尾才报错。
        long srcMb = src.length() / 1048576L;
        long outMb = srcMb * mult;
        long needMb = outMb * 2 + srcMb + 96L;
        long avail = freeMb(ctx.getCacheDir());
        note("disk", "源文件 " + (src.length() / 1048576L) + " MB · 预计峰值 " + needMb
                + " MB · 缓存分区可用 " + avail + " MB");
        if (avail > 0 && avail < needMb) {
            throw new Exception("存储空间不足：本次预计需要 " + needMb + " MB，当前可用 "
                    + avail + " MB。清理一些视频或缓存后再试。");
        }

        cb.progress("正在准备 · 自检内置解码器", -1, totalOut, -1);
        String ff = Ff.bin(ctx);
        String[] vls = firstLineArray(ff);
        if (vls == null) {
            throw new Exception("内置解码器不可用，请重新安装应用");
        }
        note("self", "自检通过 · " + (vls.length > 0 ? vls[0].trim() : "ffmpeg"));
        note("job", "输入 " + src.getName() + " · " + ii.w + "×" + ii.h
                + " · " + fmt(inFps) + " fps · " + dur + " 秒 · " + ii.frames + " 个样本 · 音轨 "
                + (ii.hasAudio ? "有" : "无"));
        note("job", "倍率 " + mult + "× → " + fmt(outFps) + " fps · 输出 " + totalOut
                + " 帧 · 分段 " + segments + " · 并发 " + workers
                + " · 单路预算 " + (int) perWorkerMb + " MB · 可用 " + mem + " MB");
        note("job", "编码参数 " + MCI_TAIL);
        cb.progress("正在准备 · 开始编码", -1, totalOut, -1);

        final List<String> errors = Collections.synchronizedList(new ArrayList<String>());
        final List<Worker> workersList = Collections.synchronizedList(new ArrayList<Worker>());
        final Assembler asm = new Assembler(new File(tempDir, "acc.ts"));
        final AtomicInteger cursor = new AtomicInteger(0);
        final AtomicReference<String> fatal = new AtomicReference<String>();
        final int segs = segments;
        final double durS = dur;
        final double inFpsF = inFps;
        final double outFpsF = outFps;
        final int totalF = totalOut;
        final long tStart = t0;
        final File srcF = src;
        final String ffF = ff;
        final File tmpF = tempDir;

        List<Thread> threads = new ArrayList<Thread>();
        for (int w = 0; w < workers; w++) {
            Thread t = new Thread(new Runnable() {
                public void run() {
                    while (!cancel && fatal.get() == null) {
                        int i = cursor.getAndIncrement();
                        if (i >= segs) {
                            return;
                        }
                        double s0 = durS * i / segs;
                        double s1 = durS * (i + 1) / segs;
                        int n = (int) Math.round(s1 * outFpsF) - (int) Math.round(s0 * outFpsF);
                        if (n <= 0) {
                            continue;
                        }
                        File out = new File(tmpF,
                                "c" + String.format(Locale.US, "%03d", Integer.valueOf(i)) + ".ts");
                        Worker wk = new Worker(ffF, srcF, out, s0, s1, inFpsF, outFpsF, n,
                                mult, errors, totalF, tStart, ii.w, ii.h, durS);
                        wk.run();
                        // 设备上 ffmpeg 可能整段崩掉（139/137）。逐级降级：越往后越保守，
                        // 最后一档索性不做运动估计，保证能出片。
                        for (int att = 1; att <= 6 && !wk.ok() && !cancel; att++) {
                            String old = wk.failure;
                            wk.failure = null;
                            wk.attempt = att;
                            note("job", "第 " + (i + 1) + " 段失败，第 " + att + " 次降级重跑（"
                                    + (old == null ? "?" : old) + "）· " + stageName(att));
                            wk.run();
                            if (old != null) {
                                errors.remove(old);
                            }
                        }
                        if (!wk.ok()) {
                            fatal.set(wk.failure != null ? wk.failure : "分段编码失败");
                            return;
                        }
                        workersList.add(wk);
                        try {
                            asm.put(i, out);
                        } catch (Throwable e) {
                            fatal.set(e.getMessage() == null ? "并合分段失败" : e.getMessage());
                            return;
                        }
                    }
                }
            }, "w" + w);
            threads.add(t);
            t.start();
        }
        for (Thread t : threads) {
            try {
                t.join();
            } catch (InterruptedException ignored) {
            }
        }
        if (cancel) {
            throw new Exception("cancel");
        }
        if (fatal.get() != null) {
            throw new Exception(fatal.get());
        }
        if (!asm.ok()) {
            throw new Exception("分段编码失败：产物不完整");
        }

        // 合并
        cb.progress("正在合并", -1, totalOut, -1);
        final File joined = asm.acc;
        note("merge", "并合 " + asm.count + " 段 → " + joined.getName()
                + " · " + (joined.length() / 1048576L) + " MB");

        // 回封音轨
        cb.progress("正在保存", -1, totalOut, -1);
        String name = "ALHFrame_" + stamp() + "_" + mult + "x.mp4";
        File outFile = new File(tempDir, name);
        Mux m = mux(ff, joined, src, outFile, ii.hasAudio ? "copy" : null, (double) totalOut / outFps);
        if (m.rc != 0 && ii.hasAudio) {
            note("merge", "音轨直拷失败，改重编码 AAC：rc=" + m.rc + " " + oneLine(m.err.toString()));
            m = mux(ff, joined, src, outFile, "aac", (double) totalOut / outFps);
        }
        if (m.rc != 0) {
            note("merge", "音轨回封再次失败，改为无音轨输出：rc=" + m.rc + " "
                    + oneLine(m.err.toString()));
            m = mux(ff, joined, src, outFile, null, (double) totalOut / outFps);
        }
        if (m.rc != 0) {
            throw new Exception("封装失败：" + tail(m.err, m.rc));
        }
        int got = countFrames(outFile);
        if (got <= 0) {
            throw new Exception("封装失败：输出文件没有视频轨");
        }
        note("merge", "封装完成 " + name + " · " + got + " 帧 · " + (outFile.length() / 1024) + " KB");
        if (Math.abs(got - totalOut) > Math.max(2, totalOut / 100)) {
            note("self", "帧数偏差：计划 " + totalOut + " 帧，实际 " + got + " 帧");
        }

        String path = publish(outFile, name);
        note("save", "落盘 " + path);
        delTree(tempDir);
        cb.done(path, got, System.currentTimeMillis() - t0);
    }

    /**
     * 有序并合器：段按序号进来，按序追加到 acc.ts 并立即删掉，
     * 所以磁盘上永远只有「已并合的部分 + 在等前面段的那几个」。
     */
    static class Assembler {
        final File acc;
        final java.util.TreeMap<Integer, File> wait = new java.util.TreeMap<Integer, File>();
        int next;
        int count;

        Assembler(File acc) {
            this.acc = acc;
        }

        synchronized void put(int idx, File f) throws Exception {
            wait.put(Integer.valueOf(idx), f);
            while (!wait.isEmpty() && wait.firstKey().intValue() == next) {
                File g = wait.remove(Integer.valueOf(next));
                append(g);
                //noinspection ResultOfMethodCallIgnored
                g.delete();
                next++;
                count++;
            }
        }

        void append(File f) throws Exception {
            FileInputStream in = new FileInputStream(f);
            FileOutputStream out = new FileOutputStream(acc, true);
            byte[] buf = new byte[262144];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            in.close();
            out.close();
        }

        synchronized boolean ok() {
            return count > 0 && acc.exists() && acc.length() > 1024 && wait.isEmpty();
        }
    }

    // ------------------------------------------------------------------ 分段 worker

    class Worker implements Runnable {
        final String ff;
        final File src;
        final File out;
        final double tStart;   // 本段起点（秒）
        final double tEnd;     // 本段终点（秒）
        final double srcFps;
        final double outFps;
        final int frames;      // 本段应产出帧数（= round(tEnd*outFps) - round(tStart*outFps)）
        final int vw, vh;      // 源宽高（用于 16 对齐补边）
        final double srcDur;   // 源总时长，用来避免在片尾之后继续读
        final int mult;
        final List<String> errors;
        final int total;
        final long t0;
        int localFrames;
        /** 本段第几次尝试：0 正常，1 单线程，2 单线程 + x264 纯 C。 */
        volatile int attempt;
        /** 本段失败原因；重跑成功会清空，所以只反映最后一次尝试的结果。 */
        volatile String failure;

        boolean ok() {
            return failure == null && out.exists() && out.length() > 1024;
        }

        Worker(String ff, File src, File out, double tStart, double tEnd, double srcFps,
               double outFps, int frames, int mult, List<String> errors, int total, long t0,
               int vw, int vh, double srcDur) {
            this.ff = ff;
            this.src = src;
            this.out = out;
            this.tStart = tStart;
            this.tEnd = tEnd;
            this.srcFps = srcFps;
            this.outFps = outFps;
            this.frames = frames;
            this.mult = mult;
            this.errors = errors;
            this.total = total;
            this.t0 = t0;
            this.vw = vw;
            this.vh = vh;
            this.srcDur = srcDur;
        }

        void report(int done) {
            int t = totalOut;
            long ms = System.currentTimeMillis() - t0;
            long eta = done > 0 ? (long) ((double) ms * (t - done) / done) : -1;
            cb.progress("正在补帧",
                    Math.min(done, t), t, eta);
        }

        public void run() {
            // 重跑：先把上一轮已经计入总量的帧数退回去。否则重跑时第二遍又从头累加，
            // 进度条冲到 100% 就再也不动，看起来像卡在最后一张。
            if (localFrames > 0) {
                doneFrames.addAndGet(-localFrames);
                localFrames = 0;
            }
            // 时间轴切段：-ss 到本段起点，多读 2 帧供末帧插值，再用 end_frame 精确截到本段帧数。
            // 不再「丢掉开头 mult 帧」——那是把一个源帧的时长白扔了，段与段之间会缺帧、总时长变短。
            String ss = fmt(tStart);
            double want = (tEnd - tStart) + 2.0 / srcFps;
            if (srcDur > 0) {
                // 读取窗绝不越过文件尾。越过之后解码器会一直吐末帧，而 minterpolate
                // 处理这些末尾重复帧时在部分机型上直接 SIGSEGV（退出码 139）。
                // 少的那一两帧交给后面的 tpad 克隆补，不经过运动估计。
                double left = Math.max(1.0 / srcFps, srcDur - tStart);
                if (left < want) {
                    want = left;
                }
            }
            String dur = fmt(want);
            if (proBin != null && attempt == 0) {
                runPro(ss, want);
                return;
            }
            if (proBin != null) {
                // 自研内核这一档没跑通，重跑时换成 ffmpeg 路径：宁可慢一点也要出片
                note("pro", out.getName() + " 改走 ffmpeg 路径重跑");
            }
            StringBuilder vf = new StringBuilder();
            // 两条禁忌，都实测过：
            // 1) 不能用 pad —— ffmpeg 9.0.2 在 1080×1620 上 pad+minterpolate 必崩，不补边反而正常；
            // 2) 不能在 minterpolate 前面挂 tpad —— 那是把克隆帧喂进运动估计，片尾必崩。
            if (attempt >= 6) {
                // 最后一档：完全不用 minterpolate，纯插帧复制。质量等同 30fps 内容放 96fps，
                // 但这条路径只碰 fps 滤镜，任何机型都能跑完。
                vf.append("fps=").append(fmt(outFps));
            } else {
                vf.append("minterpolate=fps=").append(fmt(outFps)).append(tailFor(attempt));
            }
            // 片尾追加克隆：minterpolate 在输入到文件尾时可能少吐一帧，这里补足
            vf.append(",tpad=stop_mode=clone:stop_duration=0.2");
            vf.append(",trim=end_frame=").append(frames);
            // 时间戳按帧序号重建：本段第 k 帧 = 全局第 (F0+k) 帧。
            // 用 PTS-STARTPTS 加偏移的话，片尾克隆帧会把时间戳拉开，拼接后就是一顿一顿。
            vf.append(",setpts=N/(").append(fmt(outFps)).append("*TB)+")
              .append(Math.round(tStart * outFps)).append("/(").append(fmt(outFps)).append("*TB)");

            List<String> c = new ArrayList<String>();
            c.add(ff);
            c.add("-hide_banner");
            c.add("-nostdin");
            c.add("-y");
            c.add("-ss");
            c.add(ss);
            c.add("-t");
            c.add(dur);
            c.add("-i");
            c.add(src.getAbsolutePath());
            c.add("-an");
            c.add("-vf");
            c.add(vf.toString());
            c.add("-c:v");
            c.add("libx264");
            c.add("-preset");
            c.add("veryfast");
            // 重跑时降到单线程：设备上 ffmpeg 139（SIGSEGV）多半和并发线程抢内存有关
            c.add("-threads");
            c.add(attempt == 0 ? "2" : "1");
            c.add("-crf");
            c.add("19");
            c.add("-pix_fmt");
            c.add("yuv420p");
            c.add("-progress");
            c.add("pipe:1");
            c.add("-nostats");
            // 写 mpegts：段可以直接按字节接起来，不用等全片再 concat
            c.add("-f");
            c.add("mpegts");
            c.add("-muxdelay");
            c.add("0");
            c.add("-muxpreload");
            c.add("0");
            // 无 B 帧：有 B 帧时 TS 会把首帧时间戳按重排延迟抬起，段与段就会错位一帧
            c.add("-bf");
            c.add("0");
            if (attempt >= 2) {
                // 最后兜底：关掉 libx264 的全部 SIMD，绕开某些机型上的 asm 崩溃（慢，但能出片）
                c.add("-x264-params");
                c.add("asm=c");
                // ffmpeg 自身的 SIMD 也关掉：minterpolate 的 C 路径虽慢，但不会再踩到指令集相关问题
                c.add("-cpuflags");
                c.add("0");
            }
            c.add(out.getAbsolutePath());

            note("cmd", out.getName() + " (" + fmt(tStart) + "s–" + fmt(tEnd) + "s, 输出 "
                    + frames + " 帧) " + join(c));

            Process p = null;
            final StringBuilder err = new StringBuilder();
            try {
                p = new ProcessBuilder(c).start();
                procs.add(p);
                final Process fp = p;
                Thread et = new Thread(new Runnable() {
                    public void run() {
                        try {
                            BufferedReader br = new BufferedReader(
                                    new InputStreamReader(fp.getErrorStream()), 8192);
                            String l;
                            while ((l = br.readLine()) != null) {
                                if (err.length() < (attempt >= 1 ? 8000 : 3000)) {
                                    err.append(l).append('\n');
                                }
                            }
                            br.close();
                        } catch (Throwable ignored) {
                        }
                    }
                }, "werr");
                et.start();
                BufferedReader br = new BufferedReader(
                        new InputStreamReader(p.getInputStream()), 8192);
                String l;
                while ((l = br.readLine()) != null) {
                    if (l.startsWith("frame=")) {
                        int f;
                        try {
                            f = Integer.parseInt(l.substring(6).trim());
                        } catch (Throwable ignored) {
                            continue;
                        }
                        int d = f - localFrames;
                        if (d > 0) {
                            localFrames = f;
                            report(doneFrames.addAndGet(d));
                        }
                    }
                }
                br.close();
                et.join();
                int rc = p.waitFor();
                procs.remove(p);
                note("cmd", out.getName() + " 退出码 " + rc
                        + (rc == 0 ? "" : " · stderr 尾部: " + oneLine(err.toString())));
                if (rc != 0 && !cancel) {
                    failure = "分段编码失败（本段已输出 " + localFrames + "/" + frames
                            + " 帧）：" + tail(err, rc);
                    errors.add(failure);
                } else if (rc == 0) {
                    failure = null;
                    // 退出码 0 不代表帧数齐。片尾被截或解码提前结束都会让某段少几帧，
                    // 成片里就是一次可见的卡顿，所以这里按帧数复核，缺得多就当失败重跑。
                    if (localFrames > 0 && localFrames < frames - 2) {
                        failure = "分段帧数不足（本段 " + localFrames + "/" + frames + " 帧）";
                        errors.add(failure);
                    }
                }
            } catch (Exception e) {
                if (p != null) {
                    procs.remove(p);
                }
                note("cmd", out.getName() + " 启动/读取异常: " + shortErr(e));
                if (!cancel) {
                    failure = "分段编码失败（启动阶段）：" + shortErr(e);
                    errors.add(failure);
                }
            }
        }

        /** 单引号包裹，供 /system/bin/sh 使用。 */
        String q(String s) {
            return "'" + s.replace("'", "'\\''") + "'";
        }

        /**
         * ALH Frame Standard 的分段编码：ffmpeg 解码 → 自研内核插帧 → ffmpeg 编码封装成 TS 段。
         * 三段用同一个 sh 管道串起来，按最后一段的退出码判定成败。
         */
        void runPro(String ss, double want) {
            // 倍率按真实帧率比给（可非整数：24fps→60fps 是 2.5 倍），内核按相位取整出帧，
            // 帧数严格等于 round(输入帧数×倍率)，再由 trim=end_frame 裁到本段应出的帧数。
            double ratio = outFps / srcFps;
            int nfr = Math.max(2, (int) Math.ceil(want * srcFps) + 1);
            int o;
            // 档位差别在匹配窗大小与精修开关，实测（12 帧统一口径）：
            // 360p 0.9944/0.9960；720p 0.9925/0.9952；1080p 0.9923/0.9947/0.9952；1440p 0.9925/0.9947/0.9948。
            switch (profile) {
                case 2:  o = 4194304 | 131072 | 262144; break;   // Pro：双向稠密光流（低分解算 + 全分精修 + 一致性权重）
                case 1:  o = 33980417; break;            // Standard：rad8 + 反向候选 + 亚像素
                default: o = 262144 | 32 | 524288;       // Lite：rad6 + LK，最省
            }
            if (dedup) o |= 4096;
            final int opt = o;
            int f0 = (int) Math.round(tStart * outFps);
            String vf = "trim=end_frame=" + frames
                    + ",setpts=N/(" + fmt(outFps) + "*TB)+" + f0 + "/(" + fmt(outFps) + "*TB)";
            // 匹配窗随档位放大：窗越大梯度越稳，代价按窗面积涨。4K 上窗太宽会拖很久，降到 6。
            int shortSide = Math.min(vw, vh);
            long px = (long) vw * vh;
            int win = profile == 2 ? (px > 6000000L ? 6 : 8) : (profile == 1 ? 4 : 2);
            String env = "ALH_WIN=" + win + " ";
            if (profile != 0) env += "ALH_JBU=1 ALH_BICU=1 ";
            if (profile == 2) env += "ALH_OFRES=" + (px > 1000000L ? 400 : 250) + " ALH_GNREF=3 ALH_GNA=0.6 ALH_GNS=1 ";
            if (profile == 2) win = 0;
            if (profile == 1) env += "ALH_PM=2 ALH_OB3=1 ALH_OBW=16 ";
            if (profile == 2) env += "ALH_ITER=1 ";
            StringBuilder sb = new StringBuilder();
            sb.append(q(ff)).append(" -hide_banner -nostdin -v error -ss ").append(ss)
                    .append(" -t ").append(fmt(want))
                    .append(" -i ").append(q(src.getAbsolutePath()))
                    .append(" -an -f rawvideo -pix_fmt yuv420p -");
            sb.append(" | ").append(env).append(q(proBin)).append(" - ").append(vw).append(' ').append(vh)
                    .append(' ').append(nfr).append(" - ").append(fmt(ratio)).append(' ').append(opt);
            sb.append(" | ").append(q(ff))
                    .append(" -hide_banner -nostdin -v error -y -f rawvideo -pix_fmt yuv420p -s ")
                    .append(vw).append('x').append(vh).append(" -r ").append(fmt(outFps))
                    .append(" -i - -vf ").append(q(vf))
                    .append(" -c:v libx264 -preset veryfast -threads ").append(attempt == 0 ? "2" : "1")
                    .append(" -crf 19 -pix_fmt yuv420p -f mpegts -muxdelay 0 -muxpreload 0 -bf 0 ")
                    .append(q(out.getAbsolutePath()));

            note("cmd", out.getName() + " [" + (dedup ? "Pro+去重" : "Pro") + "] ("
                    + fmt(tStart) + "s–" + fmt(tEnd) + "s, 输出 " + frames + " 帧, 输入约 "
                    + nfr + " 帧) " + sb);

            Process p = null;
            final StringBuilder err = new StringBuilder();
            try {
                p = new ProcessBuilder("/system/bin/sh", "-c", sb.toString()).start();
                procs.add(p);
                final Process fp = p;
                Thread et = new Thread(new Runnable() {
                    public void run() {
                        try {
                            BufferedReader br = new BufferedReader(
                                    new InputStreamReader(fp.getErrorStream()), 8192);
                            String l;
                            while ((l = br.readLine()) != null) {
                                if (l.startsWith("k=")) {
                                    int e = 2;
                                    while (e < l.length() && Character.isDigit(l.charAt(e))) {
                                        e++;
                                    }
                                    try {
                                        int k = Integer.parseInt(l.substring(2, e));
                                        int d = (k + 1) * mult - localFrames;
                                        if (d > 0) {
                                            localFrames += d;
                                            report(doneFrames.addAndGet(d));
                                        }
                                    } catch (Throwable ignored) {
                                    }
                                } else if (l.startsWith("wrote ")) {
                                    note("pro", out.getName() + " " + l);
                                } else if (err.length() < 3000) {
                                    err.append(l).append('\n');
                                }
                            }
                            br.close();
                        } catch (Throwable ignored) {
                        }
                    }
                }, "proerr");
                et.start();
                drain(p.getInputStream());
                et.join();
                int rc = p.waitFor();
                procs.remove(p);
                note("cmd", out.getName() + " 退出码 " + rc
                        + (rc == 0 ? "" : " · stderr 尾部: " + oneLine(err.toString())));
                if (rc != 0 && !cancel) {
                    failure = "分段编码失败（本段已输出 " + localFrames + "/" + frames + " 帧）："
                            + tail(err, rc);
                    errors.add(failure);
                } else if (rc == 0) {
                    failure = null;
                    int d = frames - localFrames;
                    if (d > 0) {
                        localFrames = frames;
                        report(doneFrames.addAndGet(d));
                    }
                }
            } catch (Exception e) {
                if (p != null) {
                    procs.remove(p);
                }
                note("cmd", out.getName() + " 启动/读取异常: " + shortErr(e));
                if (!cancel) {
                    failure = "分段编码失败（启动阶段）：" + shortErr(e);
                    errors.add(failure);
                }
            }
        }
    }

    // ------------------------------------------------------------------ 封装与落盘

    Mux mux(String ff, File joined, File src, File out, String acodec, double seconds) {
        Mux m = new Mux();
        List<String> c = new ArrayList<String>();
        c.add(ff);
        c.add("-hide_banner");
        c.add("-nostdin");
        c.add("-y");
        c.add("-i");
        c.add(joined.getAbsolutePath());
        c.add("-i");
        c.add(src.getAbsolutePath());
        c.add("-map");
        c.add("0:v:0");
        if (acodec != null) {
            c.add("-map");
            c.add("1:a:0");
            c.add("-c:a");
            c.add(acodec);
            if ("aac".equals(acodec)) {
                c.add("-b:a");
                c.add("160k");
            }
        } else {
            c.add("-an");
        }
        c.add("-c:v");
        c.add("copy");
        if (seconds > 0) {
            // 把封装长度钉在视频长度上：源音轨比视频长时，
            // 不钉的话进度条/时长会跟着音轨走，播完画面还要空转几秒。
            c.add("-t");
            c.add(fmt(seconds));
        }
        c.add("-movflags");
        c.add("+faststart");
        c.add(out.getAbsolutePath());
        m.rc = execCap(c.toArray(new String[0]), m.err);
        return m;
    }

    String publish(File tmp, String name) throws Exception {
        if (Build.VERSION.SDK_INT >= 29) {
            return publishViaMediaStore(tmp, name);
        }
        File dir = new File("/sdcard/Movies/ALH Frame");
        if (!dir.isDirectory() && !dir.mkdirs()) {
            return publishToAppDir(tmp, name);
        }
        File dst = new File(dir, name);
        copyFile(tmp, dst);
        if (dst.length() <= 0) {
            return publishToAppDir(tmp, name);
        }
        try {
            MediaScannerConnection.scanFile(ctx,
                    new String[]{dst.getAbsolutePath()},
                    new String[]{"video/mp4"}, null);
        } catch (Throwable ignored) {
        }
        return dst.getAbsolutePath();
    }

    private String publishToAppDir(File tmp, String name) throws Exception {
        File base = ctx.getExternalFilesDir(null);
        if (base == null) {
            base = ctx.getFilesDir();
        }
        File dir = new File(base, "out");
        if (!dir.isDirectory() && !dir.mkdirs()) {
            throw new Exception("无法创建导出目录");
        }
        File dst = new File(dir, name);
        copyFile(tmp, dst);
        return dst.getAbsolutePath();
    }

    private String publishViaMediaStore(File tmp, String name) throws Exception {
        ContentValues v = new ContentValues();
        v.put(MediaStore.Video.Media.DISPLAY_NAME, name);
        v.put(MediaStore.Video.Media.MIME_TYPE, "video/mp4");
        v.put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/ALH Frame");
        v.put(MediaStore.Video.Media.IS_PENDING, Integer.valueOf(1));
        Uri uri = ctx.getContentResolver().insert(
                MediaStore.Video.Media.EXTERNAL_CONTENT_URI, v);
        if (uri == null) {
            throw new Exception("无法写入媒体库");
        }
        try {
            java.io.OutputStream os = ctx.getContentResolver().openOutputStream(uri);
            if (os == null) {
                throw new Exception("无法打开媒体库");
            }
            FileInputStream in = new FileInputStream(tmp);
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                os.write(buf, 0, n);
            }
            in.close();
            os.flush();
            os.close();

            ContentValues done = new ContentValues();
            done.put(MediaStore.Video.Media.IS_PENDING, Integer.valueOf(0));
            ctx.getContentResolver().update(uri, done, null, null);

            String path = uri.toString();
            Cursor c = ctx.getContentResolver().query(uri, new String[]{"_data"},
                    null, null, null);
            if (c != null) {
                if (c.moveToFirst()) {
                    path = c.getString(0);
                }
                c.close();
            }
            return path;
        } catch (Exception e) {
            try {
                ctx.getContentResolver().delete(uri, null, null);
            } catch (Throwable ignored) {
            }
            throw e;
        }
    }

    static int countFrames(File f) {
        MediaExtractor ex = new MediaExtractor();
        try {
            ex.setDataSource(f.getAbsolutePath());
            int idx = -1;
            for (int t = 0; t < ex.getTrackCount(); t++) {
                String mime = ex.getTrackFormat(t).getString("mime");
                if (mime != null && mime.startsWith("video/")) {
                    idx = t;
                    break;
                }
            }
            if (idx < 0) {
                return -1;
            }
            ex.selectTrack(idx);
            int n = 0;
            while (ex.advance()) {
                n++;
            }
            return n;
        } catch (Throwable t) {
            return -1;
        } finally {
            try {
                ex.release();
            } catch (Throwable ignored) {
            }
        }
    }

    // ------------------------------------------------------------------ 工具

    /** 跑 `ffmpeg -version` 取第一行；旧代码只读 stderr，版本号其实全被丢掉了。 */
    String[] firstLineArray(String ff) {
        Process p = null;
        try {
            p = new ProcessBuilder(new String[]{ff, "-hide_banner", "-version"}).start();
            procs.add(p);
            drain(p.getErrorStream());
            BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream()), 8192);
            String first = br.readLine();
            while (br.readLine() != null) {
                // 读干管道，避免子进程被堵住
            }
            br.close();
            int rc = p.waitFor();
            procs.remove(p);
            if (rc != 0) {
                return null;
            }
            return new String[]{first == null ? "ffmpeg" : first.trim()};
        } catch (Exception e) {
            if (p != null) {
                procs.remove(p);
            }
            return null;
        }
    }

    int execCap(String[] cmd, StringBuilder err) {
        Process p = null;
        try {
            p = new ProcessBuilder(cmd).start();
            procs.add(p);
            drain(p.getInputStream());
            BufferedReader br = new BufferedReader(
                    new InputStreamReader(p.getErrorStream()), 8192);
            String l;
            while ((l = br.readLine()) != null) {
                if (err.length() < 4000) {
                    err.append(l).append('\n');
                }
            }
            br.close();
            int rc = p.waitFor();
            procs.remove(p);
            return rc;
        } catch (Exception e) {
            if (p != null) {
                procs.remove(p);
            }
            return -1;
        }
    }

    static void drain(final InputStream in) {
        new Thread(new Runnable() {
            public void run() {
                try {
                    byte[] b = new byte[8192];
                    while (in.read(b) > 0) {
                    }
                    in.close();
                } catch (Throwable ignored) {
                }
            }
        }).start();
    }

    static String fmt(double d) {
        String s = String.format(Locale.US, "%.3f", Double.valueOf(d));
        while (s.contains(".") && s.endsWith("0")) {
            s = s.substring(0, s.length() - 1);
        }
        if (s.endsWith(".")) {
            s = s.substring(0, s.length() - 1);
        }
        return s;
    }

    static String join(List<String> parts) {
        StringBuilder sb = new StringBuilder();
        for (String s : parts) {
            if (sb.length() > 0) {
                sb.append(' ');
            }
            sb.append(s.indexOf(' ') >= 0 ? "\"" + s + "\"" : s);
        }
        return sb.toString();
    }

    static String oneLine(String s) {
        String t = s.replace('\n', ' ').replace('\r', ' ').trim();
        while (t.indexOf("  ") >= 0) {
            t = t.replace("  ", " ");
        }
        return t.length() > 600 ? t.substring(t.length() - 600) : t;
    }

    static String tail(StringBuilder sb, int rc) {
        String s = sb == null ? "" : sb.toString().trim();
        if (s.length() > 300) {
            s = s.substring(s.length() - 300);
        }
        return "ffmpeg " + rc + (s.isEmpty() ? "" : "\n" + s);
    }

    /** 对用户只说白话；带退出码的原文留在 detail 里供复制。 */
    static String plain(String msg) {
        if (msg == null) {
            return "处理失败，请重试";
        }
        if (msg.indexOf("139") >= 0 || msg.indexOf("137") >= 0 || msg.indexOf("分段编码") >= 0) {
            return "编码进程中途退出，已自动尝试兼容模式，仍未成功。可试试降一个倍率。";
        }
        return msg.startsWith("处理") ? msg
                : "处理失败，可重试，或点右上角日志把详情发给我";
    }

    public static String shortErr(Throwable t) {
        String msg = t.getMessage();
        if (msg == null) {
            msg = "";
        }
        msg = msg.trim();
        if (msg.isEmpty()) {
            msg = t.getClass().getSimpleName();
        }
        if (msg.length() > 800) {
            msg = msg.substring(0, 800);
        }
        return msg;
    }

    void saveLastError(String msg) {
        try {
            File dir = new File(ctx.getCacheDir(), "alhframe");
            if (!dir.isDirectory() && !dir.mkdirs()) {
                return;
            }
            File f = new File(dir, "last_error.txt");
            FileOutputStream os = new FileOutputStream(f);
            os.write((stamp() + "\n" + msg + "\n").getBytes("UTF-8"));
            os.close();
        } catch (Throwable ignored) {
        }
    }

    static String stamp() {
        return new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
    }

    static void writeText(File f, String s) throws Exception {
        FileOutputStream os = new FileOutputStream(f);
        os.write(s.getBytes("UTF-8"));
        os.close();
    }

    static void copyFile(File from, File to) throws Exception {
        FileInputStream in = new FileInputStream(from);
        FileOutputStream out = new FileOutputStream(to);
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
        out.flush();
        in.close();
        out.close();
    }

    /** 启动时顺手清理上次残留；返回释放的字节数。 */
    /** 只清临时工作目录，保留 cache/in 里的源副本（用户可能还要重试）。 */
    static void wipeTemp(Context c) {
        synchronized (CACHE_LOCK) {
            delTree(new File(c.getCacheDir(), "alhframe"));
        }
    }

    static long wipeCache(Context c) {
        synchronized (CACHE_LOCK) {
            long n = 0;
            File seg = new File(c.getCacheDir(), "alhframe");
            File in = new File(c.getCacheDir(), "in");
            n += sizeTree(seg);
            n += sizeTree(in);
            delTree(seg);
            delTree(in);
            return n;
        }
    }

    static long mb(File f) {
        return sizeTree(f) / 1048576L;
    }

    /** 缓存分区剩余空间（MB），拿不到时返回 0 表示不判断。 */
    static long freeMb(File dir) {
        try {
            android.os.StatFs st = new android.os.StatFs(dir.getAbsolutePath());
            return st.getAvailableBytes() / 1048576L;
        } catch (Throwable t) {
            return 0;
        }
    }

    static long sizeTree(File f) {
        if (f == null || !f.exists()) {
            return 0;
        }
        if (f.isFile()) {
            return f.length();
        }
        File[] kids = f.listFiles();
        if (kids == null) {
            return 0;
        }
        long n = 0;
        for (File k : kids) {
            n += sizeTree(k);
        }
        return n;
    }

    static void delTree(File f) {
        if (!f.exists()) {
            return;
        }
        if (f.isDirectory()) {
            File[] kids = f.listFiles();
            if (kids != null) {
                for (File k : kids) {
                    delTree(k);
                }
            }
        }
        f.delete();
    }

    static long memAvailableMb() {
        BufferedReader br = null;
        try {
            br = new BufferedReader(new InputStreamReader(
                    new FileInputStream("/proc/meminfo")), 4096);
            String l;
            while ((l = br.readLine()) != null) {
                if (l.startsWith("MemAvailable:")) {
                    String[] p = l.trim().split("\\s+");
                    return Long.parseLong(p[1].trim()) / 1024;
                }
            }
        } catch (Throwable ignored) {
        } finally {
            if (br != null) {
                try {
                    br.close();
                } catch (Throwable ignored) {
                }
            }
        }
        return 0;
    }

    static long parseLong(String s, long def) {
        if (s == null) {
            return def;
        }
        try {
            return Long.parseLong(s.trim());
        } catch (Throwable ignored) {
            return def;
        }
    }
}