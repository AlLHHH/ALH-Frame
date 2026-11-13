package com.alh.frame;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.animation.ValueAnimator;
import android.view.MotionEvent;
import android.view.View;

/**
 * 自绘开关：48×28dp 的圆角轨道 + 白色圆钮，打开时轨道变品牌蓝、圆钮滑到右侧。
 * 不用系统 Switch：它的 thumb/track tint 在深色主题下会被 OEM 主题覆盖，
 * 而且真机上会带一条没用的文字位。
 */
public class Toggle extends View {

    public interface OnChange {
        void onChange(boolean on);
    }

    private boolean on;
    private float pos;          // 0 = 关，1 = 开（用于动画插值）
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private OnChange listener;
    private ValueAnimator anim;

    public Toggle(Context c) {
        super(c);
        setClickable(true);
        setFocusable(true);
    }

    public boolean isOn() {
        return on;
    }

    public void setOn(boolean v) {
        setOn(v, false);
    }

    public void setOn(boolean v, boolean animate) {
        on = v;
        float target = v ? 1f : 0f;
        if (!animate) {
            pos = target;
            invalidate();
            return;
        }
        if (anim != null) {
            anim.cancel();
        }
        anim = ValueAnimator.ofFloat(pos, target);
        anim.setDuration(140);
        anim.addUpdateListener(new ValueAnimator.AnimatorUpdateListener() {
            public void onAnimationUpdate(ValueAnimator a) {
                pos = (Float) a.getAnimatedValue();
                invalidate();
            }
        });
        anim.start();
    }

    public void setOnChange(OnChange l) {
        listener = l;
    }

    @Override
    protected void onMeasure(int wSpec, int hSpec) {
        int w = (int) (48 * getResources().getDisplayMetrics().density);
        int h = (int) (28 * getResources().getDisplayMetrics().density);
        setMeasuredDimension(resolveSize(w, wSpec), resolveSize(h, hSpec));
    }

    @Override
    protected void onDraw(Canvas c) {
        float w = getWidth(), h = getHeight();
        float r = h / 2f;
        // 轨道：关是灰蓝，开是品牌蓝，按 pos 插值
        int off = 0xFF303A48, acc = 0xFF3B82F6;
        int track = blend(off, acc, pos);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(track);
        rect.set(0, 0, w, h);
        c.drawRoundRect(rect, r, r, paint);
        // 圆钮
        float pad = h * 0.12f;
        float rr = r - pad;
        float cx = pad + rr + pos * (w - 2 * pad - 2 * rr);
        paint.setColor(0xFFFFFFFF);
        c.drawCircle(cx, r, rr, paint);
    }

    private static int blend(int a, int b, float t) {
        int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
        int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
        int r = (int) (ar + (br - ar) * t);
        int g = (int) (ag + (bg - ag) * t);
        int bl = (int) (ab + (bb - ab) * t);
        return 0xFF000000 | (r << 16) | (g << 8) | bl;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (!isEnabled()) {
            return false;
        }
        if (e.getAction() == MotionEvent.ACTION_UP) {
            setOn(!on, true);
            if (listener != null) {
                listener.onChange(on);
            }
            performClick();
        }
        return true;
    }

    @Override
    public boolean performClick() {
        return super.performClick();
    }
}