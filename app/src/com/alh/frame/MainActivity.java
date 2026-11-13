package com.alh.frame;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Matrix;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.media.MediaMetadataRetriever;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.TextUtils;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Locale;

public class MainActivity extends Activity {

    static final int C_BG = 0xFF0B0F14;
    static final int C_TEXT = 0xFFE8EEF7;
    static final int C_DIM = 0xFF8A95A6;
    static final int C_CARD = 0xFF141A22;
    static final int C_OFF = 0xFF212936;
    static final int C_ACC = 0xFF3B82F6;

    static final int REQ_PICK = 1001;

    static final int[] MULS = {2, 3, 4};
    final Button[] mulBtns = new Button[3];
    int mult = 2;

    ScrollView root;
    ImageView thumb;
    TextView tvThumbHint;
    TextView tvName;
    LinearLayout boxDetail;
    TextView tvRes;
    TextView tvSrc;
    TextView tvDst;
    TextView tvFmt;
    TextView tvStatus;
    TextView tvSub;
    Button btnPick;
    Button btnStart;
    /** 引擎选择：0 = ALH Frame Lite，1 = ALH Frame Standard。 */
    int engineMode = 0;
    boolean dedup = false;
    TextView tvEngine;
    TextView tvEngineNote;
    Button btnDedup;
    Button btnCancel;
    Bar bar;
    LinearLayout boxProgress;
    Engine engine;
    Engine.Info info;
    File picked;
    boolean running;
    long lastUiMs;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (Build.VERSION.SDK_INT < 29) {
            ArrayList<String> need = new ArrayList<String>();
            if (checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE)
                    != PackageManager.PERMISSION_GRANTED) {
                need.add(android.Manifest.permission.READ_EXTERNAL_STORAGE);
            }
            if (checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
                    != PackageManager.PERMISSION_GRANTED) {
                need.add(android.Manifest.permission.WRITE_EXTERNAL_STORAGE);
            }
            if (!need.isEmpty()) {
                requestPermissions(need.toArray(new String[0]), 1);
            }
        }
        Logs.append(this, "app", "启动 · Android " + Build.VERSION.RELEASE
                + " (API " + Build.VERSION.SDK_INT + ") · " + Build.MANUFACTURER + " "
                + Build.MODEL + " · " + Runtime.getRuntime().availableProcessors() + " 核");

        // 上次运行残留的分段文件可能有几百 MB～几 GB，趁用户还在选文件时先清掉，
        // 别等他按「开始处理」才在准备阶段干等。
        new Thread(new Runnable() {
            public void run() {
                try {
                    Engine.wipeCache(MainActivity.this);
                } catch (Throwable ignored) {
                }
            }
        }, "cachewipe").start();

        root = new ScrollView(this);
        root.setFillViewport(true);
        root.setBackgroundColor(C_BG);
        root.addView(buildUi());
        setContentView(root);

        if (!Terms.agreed(this)) {
            Sheet.terms(this, true);
        }
    }

    /** 页面最底部的条款入口：随内容滚动，不常驻，离操作区远。 */
    private View footer() {
        TextView t = new TextView(this);
        t.setText("免责声明与使用条款");
        t.setTextSize(12f);
        t.setTextColor(C_DIM);
        t.setGravity(android.view.Gravity.CENTER);
        t.setPadding(dp(12), dp(14), dp(12), dp(14));
        t.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                Sheet.terms(MainActivity.this, false);
            }
        });
        return t;
    }

    // ------------------------------------------------------------------ UI

    private LinearLayout buildUi() {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(20), dp(24), dp(20), dp(40));

        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(android.view.Gravity.CENTER_VERTICAL);
        TextView title = text("ALH Frame", 26, C_TEXT, true);
        title.setLetterSpacing(-0.02f);
        head.addView(title, new LinearLayout.LayoutParams(0, -2, 1f));

        TextView btnLog = new TextView(this);
        btnLog.setText("日志");
        btnLog.setTextSize(12f);
        btnLog.setTextColor(C_DIM);
        btnLog.setBackground(round(C_OFF, dp(14)));
        btnLog.setPadding(dp(13), dp(7), dp(13), dp(7));
        btnLog.setGravity(android.view.Gravity.CENTER);
        btnLog.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                Sheet.log(MainActivity.this);
            }
        });
        head.addView(btnLog, new LinearLayout.LayoutParams(-2, -2));
        col.addView(head, lp(-1, -2, 0, 0, 0, dp(20)));

        // 视频卡片
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(round(C_CARD, dp(16)));
        card.setClipToOutline(true);

        FrameLayout holder = new FrameLayout(this);
        holder.setBackgroundColor(C_OFF);
        thumb = new ImageView(this);
        thumb.setScaleType(ImageView.ScaleType.CENTER_CROP);
        holder.addView(thumb, new FrameLayout.LayoutParams(-1, -1));
        tvThumbHint = text("尚未选择文件", 13, C_DIM, false);
        FrameLayout.LayoutParams hlp = new FrameLayout.LayoutParams(-2, -2);
        hlp.gravity = 17;
        holder.addView(tvThumbHint, hlp);
        card.addView(holder, lp(-1, dp(186), 0, 0, 0, 0));

        LinearLayout meta = new LinearLayout(this);
        meta.setOrientation(LinearLayout.VERTICAL);
        meta.setPadding(dp(14), dp(12), dp(14), dp(12));
        tvName = text("未选择文件", 15, C_TEXT, true);
        tvName.setMaxLines(2);
        tvName.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        meta.addView(tvName);
        tvFmt = text("支持 MP4、MOV、MKV 等容器格式", 13, C_DIM, false);
        meta.addView(tvFmt, lp(-1, -2, 0, dp(4), 0, 0));

        boxDetail = new LinearLayout(this);
        boxDetail.setOrientation(LinearLayout.VERTICAL);
        boxDetail.setVisibility(View.GONE);
        tvRes = infoValue();
        tvSrc = infoValue();
        tvDst = infoValue();
        boxDetail.addView(infoRow("分辨率", tvRes), lp(-1, -2, 0, dp(8), 0, 0));
        boxDetail.addView(infoRow("原视频", tvSrc), lp(-1, -2, 0, dp(5), 0, 0));
        boxDetail.addView(infoRow("补帧后", tvDst), lp(-1, -2, 0, dp(5), 0, 0));
        meta.addView(boxDetail, lp(-1, -2, 0, 0, 0, 0));
        card.addView(meta);

        // 整张卡片可点：点预览区就是换文件，不用先瞄准按钮
        card.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if (!running) {
                    btnPick.performClick();
                }
            }
        });
        col.addView(card, lp(-1, -2, 0, 0, 0, dp(18)));

        btnPick = button("选择文件", false);
        btnPick.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if (running) {
                    return;
                }
                try {
                    Intent it = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    it.addCategory(Intent.CATEGORY_OPENABLE);
                    it.setType("video/*");
                    startActivityForResult(it, REQ_PICK);
                } catch (Exception e) {
                    toast("未找到可用的文件选择器");
                }
            }
        });
        col.addView(btnPick, lp(-1, dp(48), 0, 0, 0, dp(26)));

        TextView label = text("输出倍率", 12, C_DIM, false);
        label.setLetterSpacing(0.08f);
        col.addView(label, lp(-1, -2, dp(2), 0, 0, dp(10)));

        // 三段一体：外层一块 #212936 圆角底板，选中的那格填蓝，未选中的透明。
        LinearLayout seg = new LinearLayout(this);
        seg.setOrientation(LinearLayout.HORIZONTAL);
        seg.setBackground(round(C_OFF, dp(14)));
        seg.setPadding(dp(4), dp(4), dp(4), dp(4));
        for (int i = 0; i < MULS.length; i++) {
            final int idx = i;
            Button bt = button(MULS[i] + "×", false);
            bt.setBackground(round(0x00000000, dp(10)));
            bt.setTextColor(C_DIM);
            bt.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    if (running) {
                        return;
                    }
                    setMult(idx);
                }
            });
            LinearLayout.LayoutParams p = lp(0, dp(40), 0, 0, 0, 0);
            p.weight = 1f;
            seg.addView(bt, p);
            mulBtns[i] = bt;
        }
        col.addView(seg, lp(-1, -2, 0, 0, 0, dp(22)));
        setMult(0);

        // 引擎选择：下拉框，两个名字都放得下，不会换行
        LinearLayout erow = new LinearLayout(this);
        erow.setOrientation(LinearLayout.HORIZONTAL);
        erow.setGravity(android.view.Gravity.CENTER_VERTICAL);
        TextView elabel = text("补帧引擎", 13, C_DIM, false);
        elabel.setLetterSpacing(0.06f);
        erow.addView(elabel, lp(-2, -2, dp(2), 0, dp(12), 0));
        tvEngine = text(labelOf(0), 15, C_TEXT, false);
        tvEngine.setSingleLine(true);
        tvEngine.setEllipsize(TextUtils.TruncateAt.END);
        tvEngine.setGravity(android.view.Gravity.CENTER_VERTICAL);
        tvEngine.setBackground(round(C_OFF, dp(12)));
        tvEngine.setPadding(dp(14), 0, dp(14), 0);
        tvEngine.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if (!running) {
                    showEngineMenu(v);
                }
            }
        });
        LinearLayout.LayoutParams ep = lp(0, dp(46), 0, 0, 0, 0);
        ep.weight = 1f;
        erow.addView(tvEngine, ep);
        TextView arrow = text("▾", 13, C_DIM, false);
        arrow.setPadding(dp(6), 0, 0, 0);
        arrow.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if (!running && tvEngine != null) {
                    showEngineMenu(tvEngine);
                }
            }
        });
        erow.addView(arrow, lp(-2, -2, 0, 0, 0, 0));
        col.addView(erow, lp(-1, -2, 0, 0, 0, dp(8)));

        tvEngineNote = text("", 12, C_DIM, false);
        tvEngineNote.setLineSpacing(dp(2), 1f);
        col.addView(tvEngineNote, lp(-1, -2, dp(2), dp(6), 0, 0));

        btnDedup = button("去重复帧 关", false);
        btnDedup.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if (running) {
                    return;
                }
                setDedup(!dedup);
            }
        });
        col.addView(btnDedup, lp(-1, dp(44), 0, dp(6), 0, dp(24)));
        setEngine(0);

        btnStart = button("开始处理", true);
        btnStart.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                start();
            }
        });
        col.addView(btnStart, lp(-1, dp(52), 0, dp(12), 0, dp(20)));

        // 进度区只在真正开跑后出现，避免空文本把下方挤成一团。
        boxProgress = new LinearLayout(this);
        boxProgress.setOrientation(LinearLayout.VERTICAL);
        boxProgress.setBackground(round(C_CARD, dp(16)));
        boxProgress.setPadding(dp(16), dp(16), dp(16), dp(16));
        boxProgress.setVisibility(View.GONE);

        tvStatus = text("", 15, C_TEXT, true);
        boxProgress.addView(tvStatus);

        bar = new Bar(this);
        bar.setVisibility(View.GONE);
        boxProgress.addView(bar, lp(-1, dp(6), 0, dp(12), 0, 0));

        tvSub = text("", 13, C_DIM, false);
        tvSub.setLineSpacing(dp(2), 1f);
        boxProgress.addView(tvSub, lp(-1, -2, 0, dp(8), 0, 0));

        btnCancel = button("取消", false);
        btnCancel.setVisibility(View.GONE);
        btnCancel.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if (engine != null) {
                    btnCancel.setEnabled(false);
                    btnCancel.setText("正在取消…");
                    engine.cancel();
                }
            }
        });
        boxProgress.addView(btnCancel, lp(-1, dp(44), 0, dp(14), 0, 0));

        col.addView(boxProgress, lp(-1, -2, 0, 0, 0, 0));

        // 条款入口放在内容最末：要滑到底才碰到，不会误触
        col.addView(footer(), lp(-1, -2, 0, dp(30), 0, 0));
        return col;
    }

    private Button button(String s, boolean primary) {
        Button b = new Button(this);
        b.setText(s);
        b.setTextSize(15f);
        b.setAllCaps(false);
        b.setTextColor(primary ? -1 : C_TEXT);
        b.setBackground(round(primary ? C_ACC : C_OFF, dp(12)));
        b.setStateListAnimator(null);
        b.setGravity(17);
        return b;
    }

    private TextView text(String s, int size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(size);
        t.setTextColor(color);
        if (bold) {
            t.setTypeface(t.getTypeface(), 1);
        }
        return t;
    }

    private LinearLayout.LayoutParams lp(int w, int h, int l, int t, int r, int b) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, h);
        p.setMargins(l, t, r, b);
        return p;
    }

    private Drawable round(int color, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(radius);
        return d;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private String clock(long ms) {
        long s = Math.max(1000, ms) / 1000;
        if (s < 60) {
            return s + " 秒";
        }
        return s / 60 + " 分 " + s % 60 + " 秒";
    }

    private TextView infoValue() {
        return text("", 13, C_TEXT, false);
    }

    private LinearLayout infoRow(String label, TextView value) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.addView(text(label, 13, C_DIM, false), lp(dp(52), -2, 0, 0, 0, 0));
        LinearLayout.LayoutParams p = lp(0, -2, 0, 0, 0, 0);
        p.weight = 1f;
        row.addView(value, p);
        return row;
    }

    /** 选中源文件后填三行规格表。 */
    private void fillDetail(Engine.Info i) {
        tvFmt.setVisibility(View.GONE);
        boxDetail.setVisibility(View.VISIBLE);
        tvRes.setText(i.w + " × " + i.h + " · " + trim(i.durSec) + " 秒");
        tvSrc.setText(trim(i.fps) + " fps · 共 " + i.frames + " 帧");
        tvDst.setText(trim(i.fps * mult) + " fps · 共 " + (i.frames * mult) + " 帧");
    }

    /** 收起规格表，回到一行提示。 */
    private void clearDetail(String hint) {
        boxDetail.setVisibility(View.GONE);
        tvFmt.setVisibility(View.VISIBLE);
        tvFmt.setText(hint);
    }

    static String trim(double d) {
        String s = String.format(Locale.US, "%.2f", Double.valueOf(d));
        while (s.endsWith("0")) {
            s = s.substring(0, s.length() - 1);
        }
        if (s.endsWith(".")) {
            s = s.substring(0, s.length() - 1);
        }
        return s;
    }

    private void setMult(int idx) {
        mult = MULS[idx];
        for (int i = 0; i < mulBtns.length; i++) {
            if (i == idx) {
                mulBtns[i].setBackground(round(C_ACC, dp(10)));
                mulBtns[i].setTextColor(-1);
            } else {
                mulBtns[i].setBackground(round(0x00000000, dp(10)));
                mulBtns[i].setTextColor(C_DIM);
            }
        }
        if (info != null) {
            fillDetail(info);
        }
    }

    // ------------------------------------------------------------------ 流程

    // 三个类型各一列，列内按版本堆叠；code 就是 Engine 的 profile
    static final String[] ENGINE_TYPES = {"Lite", "Standard", "Pro"};
    static final String[][] ENGINE_VERS = {
            {"v1"},
            {"v1", "v2"},
            {"v1", "v2"}};
    static final int[][] ENGINE_CODES = {
            {0},
            {1, 2},
            {3, 4}};

    static int typeOf(int code) {
        return code == 0 ? 0 : (code <= 2 ? 1 : 2);
    }

    static int verOf(int code) {
        if (code == 0) return 0;
        if (code == 1) return 0;
        if (code == 2) return 1;
        if (code == 3) return 0;
        return 1;
    }

    static String labelOf(int code) {
        return "ALH Frame " + ENGINE_TYPES[typeOf(code)] + " " + ENGINE_VERS[typeOf(code)][verOf(code)];
    }

    // 自绘下拉：三列（Lite / Standard / Pro），每列按版本堆叠，避免系统弹出的白边框
    private void showEngineMenu(View anchor) {
        final android.widget.PopupWindow[] holder = new android.widget.PopupWindow[1];
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.HORIZONTAL);
        GradientDrawable bg = (GradientDrawable) round(C_CARD, dp(14));
        bg.setStroke(dp(1), 0xFF232C3A);
        box.setBackground(bg);
        box.setPadding(dp(4), dp(8), dp(4), dp(8));
        for (int t = 0; t < ENGINE_TYPES.length; t++) {
            LinearLayout col = new LinearLayout(this);
            col.setOrientation(LinearLayout.VERTICAL);
            TextView head = text(ENGINE_TYPES[t], 12, C_DIM, false);
            head.setSingleLine(true);
            head.setLetterSpacing(0.06f);
            head.setPadding(dp(10), dp(4), dp(10), dp(6));
            col.addView(head, lp(-1, -2, 0, 0, 0, 0));
            for (int v = 0; v < ENGINE_VERS[t].length; v++) {
                final int code = ENGINE_CODES[t][v];
                TextView row = text(ENGINE_VERS[t][v], 15, code == engineMode ? C_ACC : C_TEXT, false);
                row.setSingleLine(true);
                row.setPadding(dp(10), dp(11), dp(10), dp(11));
                row.setBackground(round(code == engineMode ? 0x1A3B82F6 : 0x00000000, dp(9)));
                row.setOnClickListener(new View.OnClickListener() {
                    public void onClick(View v) {
                        setEngine(code);
                        if (holder[0] != null) {
                            holder[0].dismiss();
                        }
                    }
                });
                col.addView(row, lp(-1, -2, 0, 0, 0, 0));
            }
            box.addView(col, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        }
        final android.widget.PopupWindow pop = new android.widget.PopupWindow(box,
                anchor.getWidth(), LinearLayout.LayoutParams.WRAP_CONTENT, true);
        holder[0] = pop;
        pop.setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(0x00000000));
        pop.setOutsideTouchable(true);
        pop.setElevation(dp(10));
        pop.showAsDropDown(anchor, 0, dp(6));
    }

    private void setEngine(int e) {
        engineMode = e;
        if (tvEngine != null) {
            tvEngine.setText(labelOf(e));
        }
        if (tvEngineNote != null) {
            String note;
            if (e == 0) {
                note = "基于 ffmpeg minterpolate：复杂运动更稳，推荐日常使用。";
            } else if (e == 1) {
                note = "自研内核：块匹配，速度快，细节更锐。";
            } else if (e == 2) {
                note = "自研内核：块匹配 + 亚像素精修，慢一点，运动边缘更连续。";
            } else if (e == 3) {
                note = "自研光流：流场在低分辨率上解算再逐像素融合，无方格，约为标准档一半耗时。";
            } else {
                note = "自研光流 v2：逐像素稠密流场 + 前后向遮挡判断，过渡最连续，速度最慢。";
            }
            tvEngineNote.setText(note);
        }
        if (btnDedup != null) {
            // 去重复帧是自研内核的开关，Lite 上不显示，免得看成对 ffmpeg 也能用
            btnDedup.setVisibility(e != 0 ? View.VISIBLE : View.GONE);
            btnDedup.setEnabled(e != 0);
            btnDedup.setTextColor(e != 0 ? (dedup ? -1 : C_TEXT) : C_DIM);
            btnDedup.setBackground(round(e != 0 && dedup ? C_ACC : C_OFF, dp(12)));
            btnDedup.setText(dedup ? "去重复帧 开" : "去重复帧 关");
            if (e == 0) {
                dedup = false;
                btnDedup.setText("去重复帧 关");
            }
        }
    }

    private void setDedup(boolean on) {
        dedup = on;
        if (btnDedup != null) {
            btnDedup.setText(on ? "去重复帧 开" : "去重复帧 关");
            btnDedup.setBackground(round(on ? C_ACC : C_OFF, dp(12)));
            btnDedup.setTextColor(on ? -1 : C_TEXT);
        }
    }

    private void start() {
        if (running) {
            return;
        }
        if (picked == null || !picked.canRead()) {
            toast("请先选择源文件");
            return;
        }
        if (Build.VERSION.SDK_INT < 29
                && checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{
                    android.Manifest.permission.WRITE_EXTERNAL_STORAGE,
                    android.Manifest.permission.READ_EXTERNAL_STORAGE}, 2);
            toast("需要存储权限才能写入相册");
            return;
        }

        running = true;
        btnPick.setEnabled(false);
        btnStart.setEnabled(false);
        for (Button b : mulBtns) {
            b.setEnabled(false);
        }
        if (tvEngine != null) {
            tvEngine.setEnabled(false);
            tvEngine.setTextColor(C_DIM);
        }
        if (btnDedup != null) {
            btnDedup.setEnabled(false);
        }
        boxProgress.setVisibility(View.VISIBLE);
        bar.setVisibility(View.VISIBLE);
        bar.setIndeterminate(true);
        bar.setProgress(0);
        tvStatus.setText("正在准备…");
        tvSub.setText("");
        btnCancel.setVisibility(View.VISIBLE);
        btnCancel.setEnabled(true);
        btnCancel.setText("取消");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        Keep.start(this);
        engine = new Engine(this, new Engine.Cb() {
            public void progress(final String phase, final int done, final int total,
                                 final long eta) {
                runOnUiThread(new Runnable() {
                    public void run() {
                        if (done < 0 || total <= 0) {
                            bar.setIndeterminate(true);
                            tvStatus.setText(phase);
                            tvSub.setText("");
                            return;
                        }
                        long now = SystemClock.uptimeMillis();
                        if (done < total && now - lastUiMs < 33) {
                            return;
                        }
                        lastUiMs = now;
                        bar.setIndeterminate(false);
                        bar.setProgress(Math.min(done * 1000 / total, 1000));
                        StringBuilder sb = new StringBuilder(phase);
                        sb.append(" · 第 ").append(done).append(" / ").append(total).append(" 帧");
                        if (mult > 1) {
                            sb.append("（源帧 ").append(done / mult).append('）');
                        }
                        tvStatus.setText(sb.toString());
                        StringBuilder sub = new StringBuilder();
                        sub.append(done * 100 / total).append('%');
                        if (eta > 0) {
                            sub.append(" · 预计剩余 ").append(clock(eta));
                        }
                        tvSub.setText(sub.toString());
                    }
                });
            }

            public void done(final String path, final int frames, final long ms) {
                runOnUiThread(new Runnable() {
                    public void run() {
                        finishUi();
                        bar.setIndeterminate(false);
                        bar.setProgress(1000);
                        tvStatus.setText("处理完成 · 共 " + frames + " 帧 · 耗时 " + clock(ms));
                        tvSub.setText("已保存至相册\n" + prettyPath(path));
                        toast("处理完成，已保存至相册");
                    }
                });
            }

            public void fail(final String msg, final String detail) {
                runOnUiThread(new Runnable() {
                    public void run() {
                        finishUi();
                        bar.setIndeterminate(false);
                        bar.setVisibility(View.GONE);
                        if ("已取消".equals(msg)) {
                            tvStatus.setText("已取消");
                            tvSub.setText("");
                            return;
                        }
                        tvStatus.setText("处理失败");
                        tvSub.setText("可重新开始，或改用其他编码格式的源文件。");
                        Sheet.error(MainActivity.this, "处理失败", msg, detail);
                    }
                });
            }
        });
        engine.start(picked, mult, null, engineMode, dedup);
    }

    private void finishUi() {
        Keep.stop(this);
        running = false;
        engine = null;
        btnPick.setEnabled(true);
        btnStart.setEnabled(true);
        for (Button b : mulBtns) {
            b.setEnabled(true);
        }
        if (tvEngine != null) {
            tvEngine.setEnabled(true);
            tvEngine.setTextColor(C_TEXT);
        }
        if (btnDedup != null) {
            btnDedup.setEnabled(engineMode != 0);
        }
        btnCancel.setVisibility(View.GONE);
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected void onDestroy() {
        // 用户按返回键离开时引擎线程还在跑：不停掉的话前台通知会一直挂着，再没人收尾。
        if (engine != null && running) {
            engine.cancel();
        }
        Keep.stop(this);
        super.onDestroy();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        super.onRequestPermissionsResult(code, perms, res);
        if (code == 2) {
            if (res != null && res.length > 0 && res[0] == PackageManager.PERMISSION_GRANTED) {
                start();
            } else {
                toast("未获得存储权限，输出文件无法写入相册");
            }
        }
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_PICK || res != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        final Uri uri = data.getData();
        tvName.setText("正在解析…");
        boxDetail.setVisibility(View.GONE);
        tvFmt.setVisibility(View.GONE);
        new Thread(new Runnable() {
            public void run() {
                try {
                    final File f = resolve(uri);
                    final Engine.Info in = Engine.probe(f);
                    final Bitmap bmp = firstFrame(f);
                    runOnUiThread(new Runnable() {
                        public void run() {
                            picked = f;
                            info = in;
                            tvName.setText(f.getName());
                            fillDetail(in);
                            tvThumbHint.setVisibility(View.GONE);
                            if (bmp != null) {
                                thumb.setImageBitmap(bmp);
                            }
                            btnPick.setText("更换文件");
                            tvStatus.setText("");
                            tvSub.setText("");
                            bar.setVisibility(View.GONE);
                            bar.setProgress(0);
                        }
                    });
                } catch (final Exception e) {
                    runOnUiThread(new Runnable() {
                        public void run() {
                            picked = null;
                            info = null;
                            tvName.setText("未选择文件");
                            clearDetail("读取失败：" + Engine.shortErr(e));
                            toast("无法解析该文件，请更换源文件");
                        }
                    });
                }
            }
        }, "pick").start();
    }

    private File resolve(Uri uri) throws Exception {
        if ("file".equals(uri.getScheme())) {
            File f = new File(uri.getPath());
            if (f.canRead()) {
                return f;
            }
        }
        try {
            android.database.Cursor c = getContentResolver()
                    .query(uri, new String[]{"_data"}, null, null, null);
            if (c != null) {
                if (c.moveToFirst()) {
                    String p = c.getString(0);
                    if (p != null) {
                        File f = new File(p);
                        if (f.canRead()) {
                            c.close();
                            return f;
                        }
                    }
                }
                c.close();
            }
        } catch (Throwable ignored) {
        }

        String name = "input.mp4";
        try {
            android.database.Cursor c = getContentResolver()
                    .query(uri, new String[]{"_display_name"}, null, null, null);
            if (c != null) {
                if (c.moveToFirst()) {
                    String n = c.getString(0);
                    if (n != null && !n.isEmpty()) {
                        name = n;
                    }
                }
                c.close();
            }
        } catch (Throwable ignored) {
        }

        // 缓存里只留一份源文件副本：换片前先删掉上一份，避免越选越占空间
        Engine.delTree(new File(getCacheDir(), "in"));
        File dir = new File(getCacheDir(), "in");
        dir.mkdirs();
        File dst = new File(dir, name.replaceAll("[^0-9A-Za-z._\\-\u4e00-\u9fa5]", "_"));
        InputStream in = getContentResolver().openInputStream(uri);
        if (in == null) {
            throw new Exception("无法读取源文件");
        }
        FileOutputStream out = new FileOutputStream(dst);
        byte[] buf = new byte[65536];
        long size = 0;
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
            size += n;
        }
        in.close();
        out.close();
        if (size == 0) {
            throw new Exception("源文件为空");
        }
        return dst;
    }

    private Bitmap firstFrame(File f) {
        MediaMetadataRetriever r = new MediaMetadataRetriever();
        Bitmap bmp = null;
        int rot = 0;
        try {
            r.setDataSource(f.getAbsolutePath());
            // 4K/8K 源直接取整帧可能几十上百 MB，先让系统解一张缩略图出来。
            if (Build.VERSION.SDK_INT >= 27) {
                try {
                    bmp = r.getScaledFrameAtTime(0, MediaMetadataRetriever.OPTION_CLOSEST_SYNC,
                            720, 720);
                } catch (Throwable ignored) {
                    bmp = null;
                }
            }
            if (bmp == null) {
                bmp = r.getFrameAtTime(0, MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
            }
            if (bmp == null) {
                bmp = r.getFrameAtTime(0);
            }
            // 手机拍的竖屏视频实际存的是横屏帧 + rotation 标记，
            // 这里取的帧不会自动转，不手动转一下缩略图就是躺着的。
            String rs = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION);
            if (rs != null) {
                try {
                    rot = Integer.parseInt(rs.trim());
                } catch (Throwable ignored) {
                    rot = 0;
                }
            }
        } catch (Throwable ignored) {
        } finally {
            try {
                r.release();
            } catch (Throwable ignored) {
            }
        }
        if (bmp == null) {
            return null;
        }
        if (rot == 90 || rot == 270) {
            try {
                Matrix m = new Matrix();
                m.postRotate(rot);
                Bitmap turned = Bitmap.createBitmap(bmp, 0, 0,
                        bmp.getWidth(), bmp.getHeight(), m, true);
                if (turned != bmp) {
                    bmp.recycle();
                    bmp = turned;
                }
            } catch (Throwable ignored) {
            }
        }
        int max = Math.max(bmp.getWidth(), bmp.getHeight());
        if (max > 720) {
            float k = 720f / max;
            Bitmap small = Bitmap.createScaledBitmap(bmp,
                    Math.round(bmp.getWidth() * k), Math.round(bmp.getHeight() * k), true);
            if (small != bmp) {
                bmp.recycle();
                bmp = small;
            }
        }
        return bmp;
    }

    private void errDialog(final String msg, final String detail) {
        Sheet.error(this, "处理失败", msg, detail);
    }

    static String prettyPath(String p) {
        if (p == null) {
            return "";
        }
        String s = p.replace("/storage/emulated/0/", "");
        if (s.length() > 64) {
            return "…" + s.substring(s.length() - 63);
        }
        return s;
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }
}