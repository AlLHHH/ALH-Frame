package com.alh.frame;

import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.view.View;
import android.view.animation.LinearInterpolator;

/**
 * 自绘细进度条：6dp 圆角轨道 + 品牌蓝渐变填充；
 * 无确定进度时走「流光」不定态（准备 / 合并 / 保存这些阶段）。
 */
public class Bar extends View {

    private static final int C_TRACK = 0xFF1B232F;
    private static final int C_FROM = 0xFF3B82F6;
    private static final int C_TO = 0xFF7DB3FF;

    private final Paint trackPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();

    private float value = 0f;       // 0..1
    private boolean indeterminate;
    private float sweep = 0f;
    private ValueAnimator anim;

    public Bar(Context c) {
        super(c);
        trackPaint.setColor(C_TRACK);
        fillPaint.setColor(C_FROM);
    }

    public void setProgress(int perMille) {
        value = Math.max(0f, Math.min(1f, perMille / 1000f));
        if (!indeterminate) {
            invalidate();
        }
    }

    public void setIndeterminate(boolean on) {
        if (indeterminate == on) {
            return;
        }
        indeterminate = on;
        if (on) {
            startSweep();
        } else {
            stopSweep();
        }
        invalidate();
    }

    private void startSweep() {
        stopSweep();
        anim = ValueAnimator.ofFloat(0f, 1f);
        anim.setDuration(1100);
        anim.setRepeatCount(ValueAnimator.INFINITE);
        anim.setInterpolator(new LinearInterpolator());
        anim.addUpdateListener(new ValueAnimator.AnimatorUpdateListener() {
            public void onAnimationUpdate(ValueAnimator a) {
                sweep = (Float) a.getAnimatedValue();
                invalidate();
            }
        });
        anim.start();
    }

    private void stopSweep() {
        if (anim != null) {
            anim.cancel();
            anim = null;
        }
        sweep = 0f;
    }

    @Override
    protected void onDetachedFromWindow() {
        stopSweep();
        super.onDetachedFromWindow();
    }

    @Override
    protected void onDraw(Canvas cv) {
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) {
            return;
        }
        float r = h / 2f;
        rect.set(0, 0, w, h);
        cv.drawRoundRect(rect, r, r, trackPaint);

        float end = Math.max(r * 2f, w * value);
        rect.set(0, 0, end, h);
        if (indeterminate) {
            float band = w * 0.42f;
            float x = -band + sweep * (w + band * 2f);
            rect.set(0, 0, w, h);
            cv.save();
            cv.clipRect(0, 0, w, h);
            cv.drawRoundRect(rect, r, r, trackPaint);
            fillPaint.setShader(new LinearGradient(x, 0, x + band, 0,
                    new int[]{0x00000000, C_FROM, C_TO, 0x00000000},
                    new float[]{0f, 0.35f, 0.65f, 1f}, Shader.TileMode.CLAMP));
            cv.drawRoundRect(rect, r, r, fillPaint);
            cv.restore();
        } else {
            if (value > 0f) {
                fillPaint.setShader(new LinearGradient(0, 0, w, 0,
                        new int[]{C_FROM, C_TO}, null, Shader.TileMode.CLAMP));
                cv.drawRoundRect(rect, r, r, fillPaint);
            }
        }
    }
}