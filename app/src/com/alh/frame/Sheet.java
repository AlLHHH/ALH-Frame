package com.alh.frame;

import android.app.Activity;
import android.app.Dialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

/**
 * 自绘对话框：深色卡片 + 圆角 + 等宽正文，替掉系统 AlertDialog 的老样式。
 * 用完自动 dim 背景，宽度铺满留 18dp 边距。
 */
public final class Sheet {

    static final int C_TEXT = 0xFFE8EEF7;
    static final int C_DIM = 0xFF8A95A6;
    static final int C_CARD = 0xFF141A22;
    static final int C_OFF = 0xFF212936;
    static final int C_ACC = 0xFF3B82F6;

    private Sheet() {
    }

    /** 处理失败：一句话结论 + 可复制的完整诊断。 */
    public static void error(final Activity a, String title, String msg, final String detail) {
        final String blob = TextUtils.isEmpty(detail) ? msg : msg + "\n\n" + detail;

        LinearLayout card = card(a, 0.74f);
        card.addView(title(a, title));
        TextView head = new TextView(a);
        head.setText(msg);
        head.setTextSize(13f);
        head.setTextColor(C_DIM);
        head.setLineSpacing(dp(a, 3), 1f);
        card.addView(head, margin(a, -1, -2, 0, dp(a, 6), 0, dp(a, 10)));

        TextView body = mono(a, TextUtils.isEmpty(detail) ? "没有更多细节。" : detail);
        card.addView(scroll(a, body), weight(a));

        LinearLayout row = row(a);
        row.addView(pill(a, "复制详情", true, new View.OnClickListener() {
            public void onClick(View v) {
                copy(a, "ALH Frame 诊断", blob);
                toast(a, "复制成功");
            }
        }), btnLp(a, 0));
        row.addView(pill(a, "关闭", false, new View.OnClickListener() {
            public void onClick(View v) {
                dismissOf(v);
            }
        }), btnLp(a, dp(a, 8)));
        card.addView(row, margin(a, -1, dp(a, 42), 0, dp(a, 14), 0, 0));

        show(a, card);
    }

    /** 运行日志：最近 5 MB，可复制、可清空。 */
    /** 免责声明：gate=true 是首次启动的强制同意，点「不同意」直接退出应用。 */
    public static void terms(final Activity a, final boolean gate) {
        LinearLayout card = card(a, 0.84f);
        card.addView(title(a, Terms.TITLE));

        TextView hint = new TextView(a);
        hint.setText(gate
                ? "首次使用需阅读并同意以下条款后方可继续。"
                : "当前版本条款全文。");
        hint.setTextSize(12f);
        hint.setTextColor(C_DIM);
        card.addView(hint, margin(a, -1, -2, 0, dp(a, 6), 0, 0));

        TextView body = mono(a, Terms.TEXT);
        card.addView(scroll(a, body), weight(a));

        LinearLayout row = row(a);
        if (gate) {
            row.addView(pill(a, "不同意并退出", false, new View.OnClickListener() {
                public void onClick(View v) {
                    dismissOf(v);
                    a.finish();
                }
            }), btnLp(a, 0));
            row.addView(pill(a, "同意并继续", true, new View.OnClickListener() {
                public void onClick(View v) {
                    dismissOf(v);
                    Terms.agree(a);
                }
            }), btnLp(a, dp(a, 10)));
        } else {
            row.addView(pill(a, "关闭", true, new View.OnClickListener() {
                public void onClick(View v) {
                    dismissOf(v);
                }
            }), btnLp(a, 0));
        }
        card.addView(row, margin(a, -1, dp(a, 44), 0, dp(a, 14), 0, 0));

        show(a, card, !gate);
    }

    public static void log(final Activity a) {
        LinearLayout card = card(a, 0.78f);
        card.addView(title(a, "运行日志"));

        final TextView meta = new TextView(a);
        meta.setTextSize(12f);
        meta.setTextColor(C_DIM);
        card.addView(meta, margin(a, -1, -2, 0, dp(a, 4), 0, dp(a, 10)));

        final TextView body = mono(a, "");
        final ScrollView sc = scroll(a, body);
        card.addView(sc, weight(a));

        final Runnable reload = new Runnable() {
            public void run() {
                long size = Logs.size(a);
                String text = Logs.tail(a, 96 * 1024);
                if (TextUtils.isEmpty(text)) {
                    text = "暂无日志。";
                }
                body.setText(text);
                meta.setText("上限 5 MB，超出自动裁剪。当前 " + kb(size)
                        + "，下方显示最近 " + kb(text.length()));
                sc.post(new Runnable() {
                    public void run() {
                        sc.fullScroll(View.FOCUS_DOWN);
                    }
                });
            }
        };
        reload.run();

        LinearLayout row = row(a);
        row.addView(pill(a, "复制", true, new View.OnClickListener() {
            public void onClick(View v) {
                String text = Logs.tail(a, 1024 * 1024);
                if (TextUtils.isEmpty(text)) {
                    toast(a, "日志为空");
                    return;
                }
                copy(a, "ALH Frame 日志", text);
                toast(a, "复制成功");
            }
        }), btnLp(a, 0));
        row.addView(pill(a, "清空", false, new View.OnClickListener() {
            public void onClick(View v) {
                Logs.clear(a);
                reload.run();
                toast(a, "日志已清空");
            }
        }), btnLp(a, dp(a, 8)));
        row.addView(pill(a, "关闭", false, new View.OnClickListener() {
            public void onClick(View v) {
                dismissOf(v);
            }
        }), btnLp(a, dp(a, 8)));
        card.addView(row, margin(a, -1, dp(a, 44), 0, dp(a, 14), 0, 0));

        show(a, card);
    }

    // ---------------------------------------------------------------- 组装

    private static void show(Activity a, LinearLayout card) {
        show(a, card, true);
    }

    private static void show(Activity a, LinearLayout card, boolean cancelable) {
        Dialog d = new Dialog(a, android.R.style.Theme_Translucent_NoTitleBar);
        FrameLayout root = new FrameLayout(a);
        root.setPadding(dp(a, 18), dp(a, 24), dp(a, 18), dp(a, 24));
        root.addView(card);
        d.setContentView(root);
        Window w = d.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(0));
            w.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            w.setDimAmount(0.62f);
            w.setLayout(WindowManager.LayoutParams.MATCH_PARENT,
                    WindowManager.LayoutParams.WRAP_CONTENT);
            w.setGravity(Gravity.CENTER);
        }
        card.setTag(d);
        d.setCancelable(cancelable);
        d.setCanceledOnTouchOutside(cancelable);
        d.show();
    }

    private static void dismissOf(View v) {
        View p = v;
        while (p != null) {
            Object tag = p.getTag();
            if (tag instanceof Dialog) {
                ((Dialog) tag).dismiss();
                return;
            }
            if (!(p.getParent() instanceof View)) {
                return;
            }
            p = (View) p.getParent();
        }
    }

    private static LinearLayout card(Activity a, float hRatio) {
        LinearLayout c = new LinearLayout(a);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setBackground(round(C_CARD, dp(a, 18)));
        c.setPadding(dp(a, 20), dp(a, 18), dp(a, 20), dp(a, 16));
        int h = (int) (a.getResources().getDisplayMetrics().heightPixels * hRatio);
        FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(-1, h);
        c.setLayoutParams(p);
        return c;
    }

    private static TextView title(Activity a, String s) {
        TextView t = new TextView(a);
        t.setText(s);
        t.setTextSize(18f);
        t.setTextColor(C_TEXT);
        t.setTypeface(t.getTypeface(), 1);
        return t;
    }

    private static TextView mono(Activity a, String s) {
        TextView t = new TextView(a);
        t.setText(s);
        t.setTextSize(11.5f);
        t.setTextColor(0xFFC9D5E4);
        // 系统等宽字体（Droid Sans Mono）没有中文字形，日志里中文会变成方块，
        // 所以正文用常规无衬线，靠行距和字号拉出「日志感」。
        t.setTypeface(android.graphics.Typeface.create("sans-serif", 0));
        t.setLineSpacing(dp(a, 2), 1f);
        t.setTextIsSelectable(true);
        return t;
    }

    private static ScrollView scroll(Activity a, View body) {
        ScrollView sc = new ScrollView(a);
        sc.setVerticalScrollBarEnabled(true);
        sc.setClipToPadding(false);
        sc.setPadding(0, dp(a, 10), 0, dp(a, 6));
        sc.addView(body);
        return sc;
    }

    private static LinearLayout row(Activity a) {
        LinearLayout r = new LinearLayout(a);
        r.setOrientation(LinearLayout.HORIZONTAL);
        return r;
    }

    private static Button pill(final Activity a, String s, boolean primary,
                               View.OnClickListener l) {
        Button b = new Button(a);
        b.setText(s);
        b.setTextSize(14f);
        b.setAllCaps(false);
        b.setTextColor(primary ? 0xFFFFFFFF : C_TEXT);
        b.setBackground(round(primary ? C_ACC : C_OFF, dp(a, 12)));
        b.setStateListAnimator(null);
        b.setGravity(17);
        b.setOnClickListener(l);
        return b;
    }

    private static LinearLayout.LayoutParams btnLp(Activity a, int leftMargin) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, dp(a, 44));
        p.weight = 1f;
        p.setMargins(leftMargin, 0, 0, 0);
        return p;
    }

    private static LinearLayout.LayoutParams weight(Activity a) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, 0);
        p.weight = 1f;
        return p;
    }

    private static LinearLayout.LayoutParams margin(Activity a, int w, int h,
                                                    int l, int t, int r, int b) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, h);
        p.setMargins(l, t, r, b);
        return p;
    }

    private static GradientDrawable round(int color, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(radius);
        return d;
    }

    private static int dp(Activity a, int v) {
        return Math.round(v * a.getResources().getDisplayMetrics().density);
    }

    private static String kb(long bytes) {
        if (bytes <= 0) {
            return "0 KB";
        }
        if (bytes < 1024) {
            return bytes + " B";
        }
        if (bytes < 1024 * 1024) {
            return (bytes / 1024) + " KB";
        }
        return String.format(java.util.Locale.US, "%.1f MB", bytes / 1048576.0);
    }

    private static void copy(Activity a, String label, String text) {
        ClipboardManager cm = (ClipboardManager) a.getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm != null) {
            cm.setPrimaryClip(ClipData.newPlainText(label, text));
        }
    }

    private static void toast(Activity a, String s) {
        Toast.makeText(a, s, Toast.LENGTH_SHORT).show();
    }
}