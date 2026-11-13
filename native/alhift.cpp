// ALH Interp — 自研补帧内核原型（单文件、无第三方依赖）
// 设计目标：块匹配分层光流 + 双向一致性校验 + 矢量中值滤波 + 遮挡感知双向 warp
// 用法：alhift <in.yuv> <W> <H> <N帧> <out.yuv> [倍率]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <thread>
#include <algorithm>

using std::vector;

static int W, H;
static int LEVELS;

struct Img {
    int w = 0, h = 0;
    vector<uint8_t> p;
    void alloc(int W_, int H_) { w = W_; h = H_; p.assign((size_t)w * h, 0); }
    uint8_t *row(int y) { return p.data() + (size_t)y * w; }
    const uint8_t *row(int y) const { return p.data() + (size_t)y * w; }
};

static void down2(const Img &s, Img &d) {
    d.alloc((s.w + 1) / 2, (s.h + 1) / 2);
    for (int y = 0; y < d.h; y++) {
        const uint8_t *r0 = s.row(std::min(2 * y, s.h - 1));
        const uint8_t *r1 = s.row(std::min(2 * y + 1, s.h - 1));
        uint8_t *o = d.row(y);
        for (int x = 0; x < d.w; x++) {
            int x0 = std::min(2 * x, s.w - 1), x1 = std::min(2 * x + 1, s.w - 1);
            o[x] = (uint8_t)((r0[x0] + r0[x1] + r1[x0] + r1[x1] + 2) >> 2);
        }
    }
}

struct Vec2 { float x, y; };

// 单层块匹配：以 init 矢量为中心做 ±rad 搜索，块大小 B，返回整像素最优位移
static float g_pen = 0.25f;   // 每像素每位移的惩罚（ffmpeg 等效值 0.25 灰阶/px）
static int g_win = 2;         // 窗比块每边外扩像素（B=8 → 12×12 窗；比整块 SAD 稳，又不像 2×mb 那么贵）
static int g_fit = 8;        // 全分辨率光流迭代次数（2/4/6/8 → 0.9431/0.9435/0.9437/0.9438）
static float g_fal = 0.15f;
static int g_vmed = 1;        // 稠密场矢量中值（去条纹，保留边缘）   // 全分辨率光流平滑系数
static float g_da = 0.35f;    // DIS 最细层变分平滑系数（越小越锐、越大越滑）
static int g_dit = 3;         // DIS 最细层变分迭代次数
static int g_dp = 0;
static float g_dms = 2.0f;    // DIS patch 每轮位移上限（大位移靠这个放开）
static int g_dpe = 0;         // DIS 最细层额外 patch 遍数（P=8/step=2/maxstep=0.5）
static float g_dupThr = -1.f; // 去重阈值：按噪声自动定，<0 表示还没算

// 噪声估计：拉普拉斯响应的 MAD/6 ≈ 高斯噪声 σ（灰阶）
static float y_sigma(const Img &a) {
    int w = a.w, h = a.h;
    if (w < 8 || h < 8) return 1.0f;
    vector<uint8_t> lap((size_t)(w - 2) * (h - 2));
    size_t n = 0;
    for (int y = 1; y < h - 1; y++) {
        const uint8_t *r0 = a.row(y - 1), *r1 = a.row(y), *r2 = a.row(y + 1);
        for (int x = 1; x < w - 1; x++) {
            int v = 4 * (int)r1[x] - (int)r0[x] - (int)r2[x] - (int)r1[x - 1] - (int)r1[x + 1];
            lap[n++] = (uint8_t)std::min(255, std::abs(v));
        }
    }
    if (n == 0) return 1.0f;
    std::nth_element(lap.begin(), lap.begin() + n / 2, lap.end());
    float mad = (float)lap[n / 2];
    return 1.4826f * mad / 6.0f;
}
static void match_level(const Img &a, const Img &b, int B, int rad,
                        const vector<Vec2> &init, vector<Vec2> &out,
                        vector<float> &cost, int &bw, int &bh) {
    bw = (a.w + B - 1) / B;
    bh = (a.h + B - 1) / B;
    out.assign((size_t)bw * bh, Vec2{0, 0});
    cost.assign((size_t)bw * bh, 1e9f);
    const int nx = bw, ny = bh;
    const int nthreads = std::max(1u, std::thread::hardware_concurrency());
    auto work = [&](int y0, int y1) {
        for (int by = y0; by < y1; by++) {
            for (int bx = 0; bx < nx; bx++) {
                int px = bx * B, py = by * B;
                Vec2 iv = init[(size_t)by * nx + bx];
                int cx = (int)lroundf(iv.x), cy = (int)lroundf(iv.y);
                float best = 1e9f; int bdx = 0, bdy = 0;
                // 先在初始矢量处算一次
                for (int pass = 0; pass < 2; pass++) {
                    int rx = (pass == 0) ? rad : 1;
                    for (int dy = -rx; dy <= rx; dy++) {
                        for (int dx = -rx; dx <= rx; dx++) {
                            int sx = cx + dx, sy = cy + dy;
                            if (pass == 1 && dx == 0 && dy == 0) continue;
                            // 2×B 重叠窗（对外一圈也计入代价，压制内圈因欠纹理而乱跳）
                            int x0 = std::max(0, px - g_win);
                            int y0_ = std::max(0, py - g_win);
                            int x1 = std::min(px + B + g_win, a.w);
                            int y1_ = std::min(py + B + g_win, a.h);
                            // 源块四角都必须落在 b 内，否则跳过（越界读会崩）
                            if (x0 + sx < 0 || y0_ + sy < 0 || x1 + sx > b.w || y1_ + sy > b.h) continue;
                            long sad = 0;
                            for (int y = y0_; y < y1_; y++) {
                                const uint8_t *ra = a.row(y) + x0;
                                const uint8_t *rb = b.row(y + sy) + x0 + sx;
                                for (int x = 0; x < x1 - x0; x++) {
                                    int d = (int)ra[x] - (int)rb[x];
                                    sad += d < 0 ? -d : d;
                                }
                            }
                            // 匹配代价惩罚（等效 ffmpeg 的 64×L1 预测惩罚；系数可用 ALH_PEN 覆盖）
                            long npix = (long)(y1_ - y0_) * (x1 - x0);
                            int ftop = (fabsf((float)sx) + fabsf((float)sy));
                            float c = (float)sad / (float)npix + g_pen * (float)ftop;
                            if (c < best) { best = c; bdx = sx; bdy = sy; }
                        }
                    }
                    if (pass == 0 && bdx == 0 && bdy == 0) break;
                    cx = bdx; cy = bdy;
                }
                out[(size_t)by * nx + bx] = Vec2{(float)bdx, (float)bdy};
                cost[(size_t)by * nx + bx] = best;
            }
        }
    };
    vector<std::thread> th;
    int step = std::max(1, ny / nthreads);
    for (int y = 0; y < ny; y += step) th.emplace_back(work, y, std::min(ny, y + step));
    for (auto &t : th) t.join();
}

static vector<Vec2> upsample(const vector<Vec2> &v, int bw, int bh, int nw, int nh) {
    vector<Vec2> o((size_t)nw * nh, Vec2{0, 0});
    float sx = (float)bw / (float)nw, sy = (float)bh / (float)nh;
    for (int y = 0; y < nh; y++) {
        for (int x = 0; x < nw; x++) {
            float fx = (x + 0.5f) * sx - 0.5f, fy = (y + 0.5f) * sy - 0.5f;
            int x0 = std::max(0, std::min(bw - 1, (int)floorf(fx)));
            int y0 = std::max(0, std::min(bh - 1, (int)floorf(fy)));
            o[(size_t)y * nw + x] = Vec2{v[(size_t)y0 * bw + x0].x * 2.0f, v[(size_t)y0 * bw + x0].y * 2.0f};
        }
    }
    return o;
}

// 3x3 分量中值：抹掉孤立的错矢量（低分辨率块状跳动的直接成因）
static void median3(vector<Vec2> &v, int bw, int bh) {
    vector<Vec2> o = v;
    float xs[9], ys[9];
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int yy = y + dy, xx = x + dx;
                    if (yy < 0 || xx < 0 || yy >= bh || xx >= bw) continue;
                    Vec2 t = v[(size_t)yy * bw + xx];
                    xs[n] = t.x; ys[n] = t.y; n++;
                }
            }
            std::nth_element(xs, xs + n / 2, xs + n);
            std::nth_element(ys, ys + n / 2, ys + n);
            o[(size_t)y * bw + x] = Vec2{xs[n / 2], ys[n / 2]};
        }
    }
    v.swap(o);
}

// 双线性采样矢量场（OBMC 式平滑：块中心之间插值，消除块状接缝）
static void blur3(Img &a) {
    Img t = a;
    for (int y = 1; y + 1 < a.h; y++) {
        const uint8_t *r0 = t.row(y - 1), *r1 = t.row(y), *r2 = t.row(y + 1);
        uint8_t *d = a.row(y);
        for (int x = 1; x + 1 < a.w; x++)
            d[x] = (uint8_t)((r0[x - 1] + 2 * r0[x] + r0[x + 1] + 2 * r1[x - 1] + 4 * r1[x] + 2 * r1[x + 1] + r2[x - 1] + 2 * r2[x] + r2[x + 1] + 8) >> 4);
    }
}

static inline Vec2 sample_vech(const vector<Vec2> &v, int bw, int bh, int B, int x, int y) {
    int bx = std::min(bw - 1, std::max(0, x / B));
    int by = std::min(bh - 1, std::max(0, y / B));
    return v[(size_t)by * bw + bx];
}

static inline Vec2 sample_vec_b(const vector<Vec2> &v, int bw, int bh, int B, int x, int y) {
    float fx = (float)x / B - 0.5f, fy = (float)y / B - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float ax = fx - x0, ay = fy - y0;
    auto at = [&](int bx, int by) -> Vec2 {
        bx = std::min(bw - 1, std::max(0, bx));
        by = std::min(bh - 1, std::max(0, by));
        return v[(size_t)by * bw + bx];
    };
    Vec2 p00 = at(x0, y0), p10 = at(x0 + 1, y0), p01 = at(x0, y0 + 1), p11 = at(x0 + 1, y0 + 1);
    Vec2 r;
    r.x = (p00.x * (1 - ax) + p10.x * ax) * (1 - ay) + (p01.x * (1 - ax) + p11.x * ax) * ay;
    r.y = (p00.y * (1 - ax) + p10.y * ax) * (1 - ay) + (p01.y * (1 - ax) + p11.y * ax) * ay;
    return r;
}

static inline float bilin(const Img &im, float x, float y) {
    x = std::max(0.0f, std::min((float)im.w - 1.001f, x));
    y = std::max(0.0f, std::min((float)im.h - 1.001f, y));
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    int x1 = std::min(im.w - 1, x0 + 1), y1 = std::min(im.h - 1, y0 + 1);
    float fx = x - x0, fy = y - y0;
    const uint8_t *r0 = im.row(y0), *r1 = im.row(y1);
    float a = r0[x0] * (1 - fx) + r0[x1] * fx;
    float b = r1[x0] * (1 - fx) + r1[x1] * fx;
    return a * (1 - fy) + b * fy;
}

// 亚像素细化：在 ±0.5 像素上采样 SAD，抛物线拟合位移
static float sad_at(const Img &a, const Img &b, int px, int py, int B, float vx, float vy) {
    int x1 = std::min(px + B, a.w), y1 = std::min(py + B, a.h);
    float s = 0; int n = 0;
    for (int y = py; y < y1; y++) {
        const uint8_t *ra = a.row(y);
        for (int x = px; x < x1; x++) {
            s += fabsf((float)ra[x] - bilin(b, x + vx, y + vy));
            n++;
        }
    }
    return n ? s / n : 1e9f;
}

static void refine_sub(const Img &a, const Img &b, vector<Vec2> &v, int B, int bw, int bh) {
    const int nthreads = std::max(1u, std::thread::hardware_concurrency());
    auto work = [&](int y0, int y1) {
        for (int by = y0; by < y1; by++) {
            for (int bx = 0; bx < bw; bx++) {
                int px = bx * B, py = by * B;
                Vec2 t = v[(size_t)by * bw + bx];
                auto parab = [&](float m1, float c, float p1) -> float {
                    float den = m1 - 2 * c + p1;
                    if (fabsf(den) < 1e-3f) return 0;
                    float d = 0.5f * (m1 - p1) / den;
                    return std::max(-0.5f, std::min(0.5f, d));
                };
                // 两轮抛物线：先按 ±1 像素粗定位（可修正 ±0.5），再按 ±0.5 收细（±0.25）。
                // 只做一轮 ±0.5 的话最多只能纠 0.25 像素，整数量化留下的半像素误差还在，
                // 慢速平移就会一半帧不动、一半帧跳两倍距离——画面一顿一顿的根源。
                for (int round = 0; round < 2; round++) {
                    float step = round == 0 ? 1.0f : 0.5f;
                    float c0 = sad_at(a, b, px, py, B, t.x, t.y);
                    float cx1 = sad_at(a, b, px, py, B, t.x + step, t.y);
                    float cx0 = sad_at(a, b, px, py, B, t.x - step, t.y);
                    float cy1 = sad_at(a, b, px, py, B, t.x, t.y + step);
                    float cy0 = sad_at(a, b, px, py, B, t.x, t.y - step);
                    t.x += parab(cx0, c0, cx1) * step;
                    t.y += parab(cy0, c0, cy1) * step;
                }
                v[(size_t)by * bw + bx] = t;
            }
        }
    };
    vector<std::thread> th;
    int step = std::max(1, bh / nthreads);
    for (int y = 0; y < bh; y += step) th.emplace_back(work, y, std::min(bh, y + step));
    for (auto &t : th) t.join();
}

// Gauss-Newton 亚像素迭代：在 a(x) ≈ b(x+v) 上做两轮最小二乘，比抛物线插值准得多
static void refine_lk(const Img &a, const Img &b, vector<Vec2> &v, int B, int bw, int bh, int iters) {
    const int nthreads = std::max(1u, std::thread::hardware_concurrency());
    auto work = [&](int y0, int y1) {
        for (int by = y0; by < y1; by++) {
            for (int bx = 0; bx < bw; bx++) {
                int px = bx * B, py = by * B;
                Vec2 t = v[(size_t)by * bw + bx];
                for (int it = 0; it < iters; it++) {
                    double num_x = 0, num_y = 0, den = 0;
                    for (int y = py; y < std::min(py + B, a.h); y++) {
                        const uint8_t *ra = a.row(y);
                        for (int x = px; x < std::min(px + B, a.w); x++) {
                            float bx0 = bilin(b, x + t.x, y + t.y);
                            float gx = (bilin(b, x + t.x + 1, y + t.y) - bilin(b, x + t.x - 1, y + t.y)) * 0.5f;
                            float gy = (bilin(b, x + t.x, y + t.y + 1) - bilin(b, x + t.x, y + t.y - 1)) * 0.5f;
                            double r = (double)ra[x] - bx0;
                            num_x += gx * r; num_y += gy * r; den += (double)gx * gx + (double)gy * gy;
                        }
                    }
                    if (den < 1e-3) break;
                    float dx = (float)(num_x / den), dy = (float)(num_y / den);
                    dx = std::max(-1.0f, std::min(1.0f, dx));
                    dy = std::max(-1.0f, std::min(1.0f, dy));
                    t.x += dx; t.y += dy;
                    if (fabsf(dx) < 0.02f && fabsf(dy) < 0.02f) break;
                }
                v[(size_t)by * bw + bx] = t;
            }
        }
    };
    vector<std::thread> th;
    int step = std::max(1, bh / nthreads);
    for (int y = 0; y < bh; y += step) th.emplace_back(work, y, std::min(bh, y + step));
    for (auto &t : th) t.join();
}

// 稠密 LK 精修：窗口跨 3×3 个块，窗口内每点的矢量都从双线性场采样（不是单一块矢量），
// 因此解出来的场随位置连续变化；单块 LK 只能把整块平移，接缝处的瞬时差就是「果冻」。
static void refine_dense(const Img &a, const Img &b, vector<Vec2> &v, int B, int bw, int bh, int iters) {
    const int half = (int)(B * 1.5f);
    const int SS = 4;
    const int nthreads = std::max(1u, std::thread::hardware_concurrency());
    vector<Vec2> cur = v, nxt = v;
    for (int it = 0; it < iters; it++) {
        auto work = [&](int y0, int y1) {
            for (int by = y0; by < y1; by++) {
                for (int bx = 0; bx < bw; bx++) {
                    float cx = bx * B + B * 0.5f, cy = by * B + B * 0.5f;
                    Vec2 t = sample_vec_b(cur, bw, bh, B, (int)cx, (int)cy);
                    double nx = 0, ny = 0, den = 0;
                    for (int oy = -half; oy <= half; oy += SS) {
                        for (int ox = -half; ox <= half; ox += SS) {
                            float px = cx + ox, py = cy + oy;
                            if (px < 1 || py < 1 || px > a.w - 2 || py > a.h - 2) continue;
                            Vec2 tv = sample_vec_b(cur, bw, bh, B, (int)px, (int)py);
                            float wx = px + tv.x, wy = py + tv.y;
                            if (wx < 1 || wy < 1 || wx > a.w - 2 || wy > a.h - 2) continue;
                            float bb = bilin(b, wx, wy);
                            float gx = (bilin(b, wx + 1, wy) - bilin(b, wx - 1, wy)) * 0.5f;
                            float gy = (bilin(b, wx, wy + 1) - bilin(b, wx, wy - 1)) * 0.5f;
                            double r = (double)bilin(a, px, py) - bb;
                            nx += gx * r; ny += gy * r;
                            den += (double)gx * gx + (double)gy * gy;
                        }
                    }
                    if (den > 1e-3) {
                        float dx = std::max(-0.5f, std::min(0.5f, (float)(nx / den)));
                        float dy = std::max(-0.5f, std::min(0.5f, (float)(ny / den)));
                        t.x += dx; t.y += dy;
                    }
                    nxt[(size_t)by * bw + bx] = t;
                }
            }
        };
        vector<std::thread> th;
        int step = std::max(1, bh / nthreads);
        for (int y = 0; y < bh; y += step) th.emplace_back(work, y, std::min(bh, y + step));
        for (auto &t : th) t.join();
        cur.swap(nxt);
    }
    v.swap(cur);
}

// ───────── 稠密光流（金字塔 Horn-Schunck / Gauss-Newton）─────────
// 块匹配给出的是「8×8 一格、只有整数像素」的矢量，接缝与整像素量化就是果冻的来源。
// 这里换成逐像素的稠密场：每层用 Gauss-Newton 求残差最小的位移，再做 [1 2 1] 平滑把场拉连续，
// 从 1/8 分辨率一路细化到 1/2，最后双线性放大回全分辨率。
static void gsmooth(std::vector<float> &f, int w, int h) {
    std::vector<float> t(f.size());
    for (int y = 0; y < h; y++) {
        const float *r = &f[(size_t)y * w];
        float *o = &t[(size_t)y * w];
        for (int x = 0; x < w; x++) {
            int x0 = std::max(0, x - 1), x1 = std::min(w - 1, x + 1);
            o[x] = (r[x0] + 2.0f * r[x] + r[x1]) * 0.25f;
        }
    }
    for (int y = 0; y < h; y++) {
        int y0 = std::max(0, y - 1), y1 = std::min(h - 1, y + 1);
        const float *a = &t[(size_t)y0 * w], *b = &t[(size_t)y * w], *c = &t[(size_t)y1 * w];
        float *o = &f[(size_t)y * w];
        for (int x = 0; x < w; x++) o[x] = (a[x] + 2.0f * b[x] + c[x]) * 0.25f;
    }
}

static void flow_level(const Img &a, const Img &b, std::vector<float> &u, std::vector<float> &v,
                       int iters, float alpha2) {
    const int w = a.w, h = a.h;
    const int nthreads = std::max(1u, std::thread::hardware_concurrency());
    for (int it = 0; it < iters; it++) {
        auto work = [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                for (int x = 0; x < w; x++) {
                    size_t i = (size_t)y * w + x;
                    float ux = u[i], uy = v[i];
                    float wx = x + ux, wy = y + uy;
                    if (wx < 1 || wy < 1 || wx > w - 2 || wy > h - 2) { continue; }
                    float bw = bilin(b, wx, wy);
                    float gx = (bilin(b, wx + 1, wy) - bilin(b, wx - 1, wy)) * 0.5f;
                    float gy = (bilin(b, wx, wy + 1) - bilin(b, wx, wy - 1)) * 0.5f;
                    float r = (float)a.row(y)[x] - bw;
                    float den = gx * gx + gy * gy + alpha2;
                    float du = gx * r / den, dv = gy * r / den;
                    du = std::max(-2.0f, std::min(2.0f, du));
                    dv = std::max(-2.0f, std::min(2.0f, dv));
                    u[i] = ux + du;
                    v[i] = uy + dv;
                }
            }
        };
        std::vector<std::thread> th;
        int step = std::max(1, h / nthreads);
        for (int y = 0; y < h; y += step) th.emplace_back(work, y, std::min(h, y + step));
        for (auto &t : th) t.join();
        gsmooth(u, w, h);
        gsmooth(v, w, h);
    }
}

// 从 half 分辨率金字塔求场，再双线性放大到全分辨率；返回的场语义：b(x+u) ≈ a(x)
// ---- 边缘感知的矢量场平滑（guided filter, He et al. 2010）----
// 块匹配给出的是"一块一个矢量"，直接放大就是用户看到的像素块。经典做法是：
// 先用双线性把低分辨率场放大，再用图像本身当引导做一次 guided filter，
// 让矢量在平坦区被抹平、在物体边缘保持跳变。这一步是消除块状观感的关键。
static void boxmean(const std::vector<float> &src, int w, int h, int r,
                    std::vector<float> &dst) {
    std::vector<double> ii((size_t)(w + 1) * (h + 1), 0.0);
    for (int y = 0; y < h; y++) {
        double rowsum = 0;
        for (int x = 0; x < w; x++) {
            rowsum += src[(size_t)y * w + x];
            ii[(size_t)(y + 1) * (w + 1) + (x + 1)] = ii[(size_t)y * (w + 1) + (x + 1)] + rowsum;
        }
    }
    dst.assign((size_t)w * h, 0.0f);
    for (int y = 0; y < h; y++) {
        int y0 = std::max(0, y - r), y1 = std::min(h - 1, y + r);
        for (int x = 0; x < w; x++) {
            int x0 = std::max(0, x - r), x1 = std::min(w - 1, x + r);
            double s = ii[(size_t)(y1 + 1) * (w + 1) + (x1 + 1)] - ii[(size_t)y0 * (w + 1) + (x1 + 1)]
                     - ii[(size_t)(y1 + 1) * (w + 1) + x0] + ii[(size_t)y0 * (w + 1) + x0];
            dst[(size_t)y * w + x] = (float)(s / ((double)(y1 - y0 + 1) * (x1 - x0 + 1)));
        }
    }
}

static void guided_filter(const Img &guide, std::vector<float> &p, int w, int h,
                          int r, float eps) {
    std::vector<float> I((size_t)w * h);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = guide.row(y);
        for (int x = 0; x < w; x++) I[(size_t)y * w + x] = (float)row[x];
    }
    std::vector<float> mI, mp, mIp, mII, t1((size_t)w * h), t2((size_t)w * h);
    boxmean(I, w, h, r, mI);
    boxmean(p, w, h, r, mp);
    for (size_t i = 0; i < I.size(); i++) { t1[i] = I[i] * p[i]; t2[i] = I[i] * I[i]; }
    boxmean(t1, w, h, r, mIp);
    boxmean(t2, w, h, r, mII);
    std::vector<float> A((size_t)w * h), B((size_t)w * h);
    for (size_t i = 0; i < I.size(); i++) {
        float var = mII[i] - mI[i] * mI[i];
        float cov = mIp[i] - mI[i] * mp[i];
        float a = cov / (var + eps);
        A[i] = a;
        B[i] = mp[i] - a * mI[i];
    }
    std::vector<float> mA, mB;
    boxmean(A, w, h, r, mA);
    boxmean(B, w, h, r, mB);
    for (size_t i = 0; i < p.size(); i++) p[i] = mA[i] * I[i] + mB[i];
}

// ---- v2 主流程：低分辨率块匹配 → 双线性放大 → guided filter → 全分辨率光流细化 ----
// 全程不在全分辨率上做"一块一个矢量"，所以不会出现方格。
static void flow2_dense(const std::vector<Img> &pa, const std::vector<Img> &pb,
                        int W, int H, std::vector<Vec2> &out,
                        const std::vector<Vec2> &seedHalf, int sbw, int sbh, int B) {
    std::vector<float> u((size_t)W * H), v((size_t)W * H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            Vec2 s = sample_vec_b(seedHalf, sbw, sbh, B, x * 0.5f + 0.25f, y * 0.5f + 0.25f);
            u[(size_t)y * W + x] = s.x * 2.0f;
            v[(size_t)y * W + x] = s.y * 2.0f;
        }
    }
    static const float E1 = getenv("ALH_G1") ? (float)atof(getenv("ALH_G1")) : 30.0f;
    static const float E2 = getenv("ALH_G2") ? (float)atof(getenv("ALH_G2")) : 20.0f;
    static const float A1 = getenv("ALH_A1") ? (float)atof(getenv("ALH_A1")) : 0.6f;
    static const float A2 = getenv("ALH_A2") ? (float)atof(getenv("ALH_A2")) : 0.3f;
    guided_filter(pa[0], u, W, H, 4, E1);
    guided_filter(pa[0], v, W, H, 4, E1);
    flow_level(pa[0], pb[0], u, v, 3, A1);
    guided_filter(pa[0], u, W, H, 3, E2);
    guided_filter(pa[0], v, W, H, 3, E2);
    flow_level(pa[0], pb[0], u, v, 2, A2);
    out.assign((size_t)W * H, Vec2{0, 0});
    for (size_t i = 0; i < out.size(); i++) out[i] = Vec2{u[i], v[i]};
}

// 候选竞争：每个像素不只信一个矢量，把「光流、块匹配、反向场推出的矢量、四邻微调」
// 都代进对称残差里比一遍，谁让两帧对得上就用谁。遮挡边缘靠这条不需要额外的分割。
static void compete(const Img &a, const Img &b, int W, int H, std::vector<Vec2> &f,
                    const std::vector<Vec2> &bf, int bw, int bh, int B,
                    const std::vector<Vec2> &rev, int rw, int rh) {
    std::vector<Vec2> out(f.size());
    const float T = 0.5f;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t i = (size_t)y * W + x;
            Vec2 c[10];
            int n = 0;
            c[n++] = f[i];
            if (bw > 0 && bh > 0) c[n++] = sample_vec_b(bf, bw, bh, B, x, y);
            if (rw > 0 && rh > 0) {
                Vec2 rv = sample_vec_b(rev, rw, rh, B, x, y);
                c[n++] = Vec2{-0.5f * rv.x * 2.0f * 0.5f - rv.x * 0.0f, -0.5f * rv.y};
            }
            c[n++] = Vec2{f[i].x + 0.5f, f[i].y};
            c[n++] = Vec2{f[i].x, f[i].y + 0.5f};
            c[n++] = Vec2{f[i].x - 0.5f, f[i].y};
            c[n++] = Vec2{f[i].x + 0.5f, f[i].y + 0.5f};
            c[n++] = Vec2{f[i].x - 0.5f, f[i].y - 0.5f};
            float best = 1e9f; Vec2 bv = f[i];
            for (int k = 0; k < n; k++) {
                float p0 = bilin(a, x - c[k].x * T, y - c[k].y * T);
                float p1 = bilin(b, x + c[k].x * (1.0f - T), y + c[k].y * (1.0f - T));
                if (p0 < 0 || p1 < 0) continue;
                static const float PEN = getenv("ALH_CPEN") ? (float)atof(getenv("ALH_CPEN")) : 0.04f;
                float cost = std::fabs(p0 - p1) + PEN * (std::fabs(c[k].x) + std::fabs(c[k].y));
                if (cost < best) { best = cost; bv = c[k]; }
            }
            out[i] = bv;
        }
    }
    f.swap(out);
}

// 逐块仲裁：光流把场拉连续，但遮挡边缘处块匹配的整块矢量反而更准。
// 每 8×8 块比一次「对称残差」，块匹配更好就把这整块的矢量写回去。
static float mid_res(const Img &a, const Img &b, int x, int y, Vec2 vv) {
    float p0 = bilin(a, x - vv.x * 0.5f, y - vv.y * 0.5f);
    float p1 = bilin(b, x + vv.x * 0.5f, y + vv.y * 0.5f);
    if (p0 < 0 || p1 < 0) return 255.0f;
    return std::fabs(p0 - p1);
}

static void arbitrate(const Img &a, const Img &b, int W, int H,
                      std::vector<Vec2> &dense, const std::vector<Vec2> &bf,
                      int bw, int bh, int B) {
    if (bw <= 0 || bh <= 0) return;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            Vec2 bv = bf[(size_t)by * bw + bx];
            int cx = bx * B + B / 2, cy = by * B + B / 2;
            if (cx >= W || cy >= H) continue;
            Vec2 dv = dense[(size_t)cy * W + cx];
            float rd = 0, rb = 0; int n = 0;
            for (int k = 0; k < 5; k++) {
                int x = bv.x == bv.x ? std::min(W - 1, std::max(0, cx + (k == 1 ? -2 : (k == 2 ? 2 : 0))))
                                     : cx;
                int y = std::min(H - 1, std::max(0, cy + (k == 3 ? -2 : (k == 4 ? 2 : 0))));
                rd += mid_res(a, b, x, y, dv);
                rb += mid_res(a, b, x, y, bv);
                n++;
            }
            if (n == 0) continue;
            rd /= n; rb /= n;
            static const float ARB_T = getenv("ALH_ARBT") ? (float)atof(getenv("ALH_ARBT")) : 0.98f;
            if (rb < rd * ARB_T) {
                for (int y = by * B; y < std::min(H, by * B + B); y++)
                    for (int x = bx * B; x < std::min(W, bx * B + B); x++)
                        dense[(size_t)y * W + x] = bv;
            }
        }
    }
}

// 分量中值：3×3 邻域对 u、v 分别取中值。比矢量中值快一个数量级（不用算两两距离），
// 目的只是抹掉细长条纹；矢量中值实测只多 0.0002 SSIM，不值那 100 ms/帧。
static void vmedian3(std::vector<Vec2> &f, int w, int h) {
    std::vector<Vec2> src = f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float cu[9], cv[9]; int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = std::max(0, std::min(w - 1, x + dx));
                    int yy = std::max(0, std::min(h - 1, y + dy));
                    Vec2 t = src[(size_t)yy * w + xx];
                    cu[n] = t.x; cv[n] = t.y; n++;
                }
            std::sort(cu, cu + 9); std::sort(cv, cv + 9);
            f[(size_t)y * w + x] = Vec2{cu[4], cv[4]};
        }
}

static void flow_dense(const std::vector<Img> &pa, const std::vector<Img> &pb, int W, int H,
                       std::vector<Vec2> &out, const std::vector<Vec2> *seed = nullptr,
                       int sbw = 0, int sbh = 0, int sB = 8) {
    std::vector<float> u, v;
    int pw = 0, ph = 0;
    for (int l = LEVELS - 1; l >= 1; l--) {
        int w = pa[l].w, h = pa[l].h;
        if (u.empty()) {
            u.assign((size_t)w * h, 0.0f); v.assign((size_t)w * h, 0.0f);
            if (seed && !seed->empty()) {
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        Vec2 sv = sample_vec_b(*seed, sbw, sbh, sB, x * 2, y * 2);
                        u[(size_t)y * w + x] = sv.x * 0.5f;
                        v[(size_t)y * w + x] = sv.y * 0.5f;
                    }
            }
        }
        else {
            // 上一层的场放大两倍：粗层的 1 px 等于细层的 2 px
            std::vector<float> nu((size_t)w * h), nv((size_t)w * h);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    float sx = (x + 0.5f) * 0.5f - 0.5f, sy = (y + 0.5f) * 0.5f - 0.5f;
                    int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
                    float ax = sx - x0, ay = sy - y0;
                    auto at = [&](const std::vector<float> &f, int xx, int yy) {
                        xx = std::max(0, std::min(pw - 1, xx));
                        yy = std::max(0, std::min(ph - 1, yy));
                        return f[(size_t)yy * pw + xx];
                    };
                    float a00 = at(u, x0, y0), a10 = at(u, x0 + 1, y0), a01 = at(u, x0, y0 + 1), a11 = at(u, x0 + 1, y0 + 1);
                    float b00 = at(v, x0, y0), b10 = at(v, x0 + 1, y0), b01 = at(v, x0, y0 + 1), b11 = at(v, x0 + 1, y0 + 1);
                    nu[(size_t)y * w + x] = 2.0f * ((a00 * (1 - ax) + a10 * ax) * (1 - ay) + (a01 * (1 - ax) + a11 * ax) * ay);
                    nv[(size_t)y * w + x] = 2.0f * ((b00 * (1 - ax) + b10 * ax) * (1 - ay) + (b01 * (1 - ax) + b11 * ax) * ay);
                }
            }
            u.swap(nu); v.swap(nv);
        }
        pw = w; ph = h;
        flow_level(pa[l], pb[l], u, v, 4, 2.0f);
        flow_level(pa[l], pb[l], u, v, 2, 0.5f);
    }
    // 放大到全分辨率后，再在全分辨率上做两轮轻量高斯牛顿：放大插值会磨掉细节，
    // 这一步把它拉回真实位置（只平滑一次，别把边缘糊了）
    {
        std::vector<float> fu((size_t)W * H), fv((size_t)W * H);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float sx = (x + 0.5f) * 0.5f - 0.5f, sy = (y + 0.5f) * 0.5f - 0.5f;
                int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
                float ax = sx - x0, ay = sy - y0;
                auto at = [&](int xx, int yy, bool horiz) {
                    xx = std::max(0, std::min(pw - 1, xx));
                    yy = std::max(0, std::min(ph - 1, yy));
                    size_t i = (size_t)yy * pw + xx;
                    return horiz ? u[i] : v[i];
                };
                fu[(size_t)y * W + x] = 2.0f * ((at(x0, y0, 1) * (1 - ax) + at(x0 + 1, y0, 1) * ax) * (1 - ay)
                                              + (at(x0, y0 + 1, 1) * (1 - ax) + at(x0 + 1, y0 + 1, 1) * ax) * ay);
                fv[(size_t)y * W + x] = 2.0f * ((at(x0, y0, 0) * (1 - ax) + at(x0 + 1, y0, 0) * ax) * (1 - ay)
                                              + (at(x0, y0 + 1, 0) * (1 - ax) + at(x0 + 1, y0 + 1, 0) * ax) * ay);
            }
        for (int it = 0; it < g_fit; it++) {
            flow_level(pa[0], pb[0], fu, fv, 1, g_fal);
        }
        if (g_vmed) {
            std::vector<Vec2> tmp((size_t)W * H);
            for (size_t i = 0; i < tmp.size(); i++) tmp[i] = Vec2{fu[i], fv[i]};
            vmedian3(tmp, W, H);
            for (size_t i = 0; i < tmp.size(); i++) { fu[i] = tmp[i].x; fv[i] = tmp[i].y; }
        }
        out.assign((size_t)W * H, Vec2{0, 0});
        for (size_t i = 0; i < out.size(); i++) out[i] = Vec2{fu[i], fv[i]};
        return;
    }
    // 放大到全分辨率
    out.assign((size_t)W * H, Vec2{0, 0});
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float sx = (x + 0.5f) * 0.5f - 0.5f, sy = (y + 0.5f) * 0.5f - 0.5f;
            int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
            float ax = sx - x0, ay = sy - y0;
            auto at = [&](int xx, int yy, bool horiz) {
                xx = std::max(0, std::min(pw - 1, xx));
                yy = std::max(0, std::min(ph - 1, yy));
                size_t i = (size_t)yy * pw + xx;
                return horiz ? u[i] : v[i];
            };
            float uu = (at(x0, y0, 1) * (1 - ax) + at(x0 + 1, y0, 1) * ax) * (1 - ay)
                     + (at(x0, y0 + 1, 1) * (1 - ax) + at(x0 + 1, y0 + 1, 1) * ax) * ay;
            float vv = (at(x0, y0, 0) * (1 - ax) + at(x0 + 1, y0, 0) * ax) * (1 - ay)
                     + (at(x0, y0 + 1, 0) * (1 - ax) + at(x0 + 1, y0 + 1, 0) * ax) * ay;
            out[(size_t)y * W + x] = Vec2{uu * 2.0f, vv * 2.0f};
        }
    }
}


// ===================== 变分光流：低分辨率 SOR 求解 + 双线性放大 =====================
// 流场在短边 ~250px 的尺度上解（高清档 400），解完再放大回全分辨率。
// 场本身是逐像素连续的，所以画面上不会有方格；低分辨率解算又让它在手机上够快。

static int g_ofres = 250;
static int g_hsout = 6;
static int g_hsin = 12;
static float g_hsa = 0.012f;   // 平滑项 alpha^2
static int g_hsm = 0;           // 每次 SOR 后是否再做一次平滑（默认关，交给中值滤波）

static void resize_area(const Img &s, Img &d, int nw, int nh) {
    d.w = nw; d.h = nh; d.p.assign((size_t)nw * nh, 0);
    float sx = (float)s.w / nw, sy = (float)s.h / nh;
    for (int y = 0; y < nh; y++) {
        int y0 = (int)(y * sy), y1 = std::min(s.h, (int)((y + 1) * sy));
        if (y1 <= y0) y1 = std::min(s.h, y0 + 1);
        for (int x = 0; x < nw; x++) {
            int x0 = (int)(x * sx), x1 = std::min(s.w, (int)((x + 1) * sx));
            if (x1 <= x0) x1 = std::min(s.w, x0 + 1);
            int acc = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) { acc += s.row(yy)[xx]; n++; }
            d.p[(size_t)y * nw + x] = (uint8_t)(n ? acc / n : 0);
        }
    }
}

static vector<Vec2> upsample_scaled(const vector<Vec2> &v, int bw, int bh, int nw, int nh, float mag) {
    vector<Vec2> o((size_t)nw * nh, Vec2{0, 0});
    for (int y = 0; y < nh; y++) {
        float fy = ((y + 0.5f) * bh / nh) - 0.5f;
        int y0 = std::max(0, std::min(bh - 1, (int)floorf(fy)));
        int y1 = std::min(bh - 1, y0 + 1);
        float ay = std::max(0.f, std::min(1.f, fy - y0));
        for (int x = 0; x < nw; x++) {
            float fx = ((x + 0.5f) * bw / nw) - 0.5f;
            int x0 = std::max(0, std::min(bw - 1, (int)floorf(fx)));
            int x1 = std::min(bw - 1, x0 + 1);
            float ax = std::max(0.f, std::min(1.f, fx - x0));
            Vec2 v00 = v[(size_t)y0 * bw + x0], v01 = v[(size_t)y0 * bw + x1];
            Vec2 v10 = v[(size_t)y1 * bw + x0], v11 = v[(size_t)y1 * bw + x1];
            float ux = (v00.x * (1 - ax) + v01.x * ax) * (1 - ay) + (v10.x * (1 - ax) + v11.x * ax) * ay;
            float uy = (v00.y * (1 - ax) + v01.y * ax) * (1 - ay) + (v10.y * (1 - ax) + v11.y * ax) * ay;
            o[(size_t)y * nw + x] = Vec2{ux * mag, uy * mag};
        }
    }
    return o;
}

// 一次 warp + 导数 + SOR 松弛；u/v 是绝对位移（像素）
static void hs_sor(const Img &a, const Img &b, std::vector<float> &u, std::vector<float> &v,
                   int inner, float alpha2, float omega) {
    const int w = a.w, h = a.h;
    std::vector<float> Ix((size_t)w * h), Iy((size_t)w * h), It((size_t)w * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            float av = a.row(y)[x];
            float bx = x + u[i], by = y + v[i];
            float bv = bilin(b, bx, by);
            float gxa = 0.5f * (bilin(a, x + 1.0f, (float)y) - bilin(a, x - 1.0f, (float)y));
            float gya = 0.5f * (bilin(a, (float)x, y + 1.0f) - bilin(a, (float)x, y - 1.0f));
            float gxb = 0.5f * (bilin(b, bx + 1.0f, by) - bilin(b, bx - 1.0f, by));
            float gyb = 0.5f * (bilin(b, bx, by + 1.0f) - bilin(b, bx, by - 1.0f));
            Ix[i] = 0.5f * (gxa + gxb);
            Iy[i] = 0.5f * (gya + gyb);
            It[i] = bv - av;   // 线性化 b(x+u) = a(x)：Ix*u + (b-a) = 0
        }
    }
    for (int it = 0; it < inner; it++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                float us = 0, vs = 0; int n = 0;
                if (x > 0) { us += u[i - 1]; vs += v[i - 1]; n++; }
                if (x + 1 < w) { us += u[i + 1]; vs += v[i + 1]; n++; }
                if (y > 0) { us += u[i - w]; vs += v[i - w]; n++; }
                if (y + 1 < h) { us += u[i + w]; vs += v[i + w]; n++; }
                if (!n) continue;
                float ub = us / n, vb = vs / n;
                float d = Ix[i] * ub + Iy[i] * vb + It[i];
                float den = alpha2 + Ix[i] * Ix[i] + Iy[i] * Iy[i];
                float nu = ub - Ix[i] * d / den;
                float nv = vb - Iy[i] * d / den;
                u[i] += omega * (nu - u[i]);
                v[i] += omega * (nv - v[i]);
                if (u[i] > 24.f) u[i] = 24.f;
                if (u[i] < -24.f) u[i] = -24.f;
                if (v[i] > 24.f) v[i] = 24.f;
                if (v[i] < -24.f) v[i] = -24.f;
            }
        }
        if (g_hsm) { gsmooth(u, w, h); gsmooth(v, w, h); }
    }
}


// 低分辨率逐像素光流：把两帧缩到短边 ~g_ofres，在那一层跑逐像素 Gauss-Newton 金字塔，
// 解完再双线性放大回全分辨率。流场尺度远小于画面，是这个方案能在手机上跑住的原因；
// 放大后的场仍是逐像素连续的，画面上不会出现方格。

// 两候选逐像素择优：块场在纹理上准，稠密场在弱纹理/大位移上稳。
// 每个像素用对称残差选一个，不做整块回退，所以画面上不会出现方格。
static void blend_candidates(const Img &a, const Img &b, int W, int H,
                     std::vector<Vec2> &dense,
                     const std::vector<Vec2> &blk, int bw, int bh, int sB) {
    std::vector<Vec2> out((size_t)W * H);
    const float t = 0.5f;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t i = (size_t)y * W + x;
            Vec2 vd = dense[i];
            Vec2 vb = sample_vec_b(blk, bw, bh, sB, (float)x, (float)y);
            float cd = fabsf(bilin(a, x - vd.x * t, y - vd.y * t) - bilin(b, x + vd.x * (1 - t), y + vd.y * (1 - t)));
            float cb = fabsf(bilin(a, x - vb.x * t, y - vb.y * t) - bilin(b, x + vb.x * (1 - t), y + vb.y * (1 - t)));
            out[i] = (cb < cd) ? vb : vd;
        }
    }
    dense.swap(out);
}

static void flow_gn_lowres(const Img &A, const Img &B, std::vector<Vec2> &out) {
    float sc = (float)g_ofres / (float)std::max(1, std::min(A.w, A.h));
    if (sc > 1.f) sc = 1.f;
    int tw = std::max(24, (int)(A.w * sc)), th = std::max(24, (int)(A.h * sc));
    std::vector<Img> qa(LEVELS), qb(LEVELS);
    resize_area(A, qa[0], tw, th);
    resize_area(B, qb[0], tw, th);
    for (int l = 1; l < LEVELS; l++) { down2(qa[l - 1], qa[l]); down2(qb[l - 1], qb[l]); }
    std::vector<Vec2> small;
    flow_dense(qa, qb, tw, th, small, nullptr, 0, 0, 8);
    if ((int)small.size() != tw * th) small.assign((size_t)tw * th, Vec2{0, 0});
    float mag = (float)A.w / (float)tw;
    out = upsample_scaled(small, tw, th, A.w, A.h, mag);
}

static void flow_hs(const Img &A, const Img &B, std::vector<Vec2> &out) {
    float sc = (float)g_ofres / (float)std::max(1, std::min(A.w, A.h));
    if (sc > 1.f) sc = 1.f;
    int tw = std::max(16, (int)(A.w * sc)), th = std::max(16, (int)(A.h * sc));
    Img a, b;
    resize_area(A, a, tw, th);
    resize_area(B, b, tw, th);

    std::vector<Img> pa, pb;
    pa.push_back(a); pb.push_back(b);
    while ((int)pa.size() < 6 && std::min(pa.back().w, pa.back().h) > 40) {
        Img na, nb;
        down2(pa.back(), na); down2(pb.back(), nb);
        pa.push_back(na); pb.push_back(nb);
    }
    vector<Vec2> field;
    int pw = 0, ph = 0;
    for (int l = (int)pa.size() - 1; l >= 0; l--) {
        int w = pa[l].w, h = pa[l].h;
        vector<Vec2> init((size_t)w * h, Vec2{0, 0});
        if (pw && ph) init = upsample_scaled(field, pw, ph, w, h, 2.0f);
        std::vector<float> u((size_t)w * h), v((size_t)w * h);
        for (size_t i = 0; i < u.size(); i++) { u[i] = init[i].x; v[i] = init[i].y; }
        for (int o = 0; o < g_hsout; o++) {
            hs_sor(pa[l], pb[l], u, v, g_hsin, g_hsa, 1.6f);
            vector<Vec2> tmp((size_t)w * h);
            for (size_t i = 0; i < tmp.size(); i++) tmp[i] = Vec2{u[i], v[i]};
            median3(tmp, w, h);
            for (size_t i = 0; i < tmp.size(); i++) { u[i] = tmp[i].x; v[i] = tmp[i].y; }
        }
        field.resize((size_t)w * h);
        for (size_t i = 0; i < field.size(); i++) field[i] = Vec2{u[i], v[i]};
        pw = w; ph = h;
    }
    {
        // 诊断：带流 warp 后的残差 vs 不带流
        double r0 = 0, r1 = 0; long cnt = 0;
        for (int y = 1; y + 1 < a.h; y++)
            for (int x = 1; x + 1 < a.w; x++) {
                size_t i = (size_t)y * a.w + x;
                float ax = a.row(y)[x];
                r0 += fabs(ax - (float)b.row(y)[x]);
                r1 += fabs(ax - bilin(b, x + field[i].x, y + field[i].y));
                cnt++;
            }
        if (cnt) fprintf(stderr, "hs resid noflow=%.2f withflow=%.2f\n", r0 / cnt, r1 / cnt);
        double su = 0, sv = 0, sa = 0;
        for (auto &q : field) { su += fabs(q.x); sv += fabs(q.y); sa += sqrt(q.x * q.x + q.y * q.y); }
        double n = field.empty() ? 1 : (double)field.size();
        fprintf(stderr, "hs field %dx%d mean|u|=%.3f mean|v|=%.3f mean|f|=%.3f\n", tw, th, su / n, sv / n, sa / n);
    }
    out = upsample_scaled(field, tw, th, A.w, A.h, 1.0f);
}

// ---------------------------------------------------------------------------
// DIS 式稠密光流：多尺度 patch 逆向搜索（Gauss-Newton 用块内 Hessian 预解）
// + 每层变分细化（逐像素 GN + 场平滑）。语义与其它场一致：b(x+u) ≈ a(x)。
// ---------------------------------------------------------------------------
static void upf(const std::vector<float> &s, int sw, int sh, std::vector<float> &d, int dw, int dh) {
    d.assign((size_t)dw * dh, 0.f);
    for (int y = 0; y < dh; y++) {
        float fy = ((y + 0.5f) * sh / dh) - 0.5f;
        int y0 = std::max(0, std::min(sh - 1, (int)floorf(fy)));
        int y1 = std::min(sh - 1, y0 + 1);
        float ay = std::max(0.f, std::min(1.f, fy - y0));
        for (int x = 0; x < dw; x++) {
            float fx = ((x + 0.5f) * sw / dw) - 0.5f;
            int x0 = std::max(0, std::min(sw - 1, (int)floorf(fx)));
            int x1 = std::min(sw - 1, x0 + 1);
            float ax = std::max(0.f, std::min(1.f, fx - x0));
            float v00 = s[(size_t)y0 * sw + x0], v01 = s[(size_t)y0 * sw + x1];
            float v10 = s[(size_t)y1 * sw + x0], v11 = s[(size_t)y1 * sw + x1];
            d[(size_t)y * dw + x] = (v00 * (1 - ax) + v01 * ax) * (1 - ay) + (v10 * (1 - ax) + v11 * ax) * ay;
        }
    }
}

static void tri_smooth(std::vector<float> &f, int w, int h) { gsmooth(f, w, h); }

// 一层的 patch 逆向搜索：网格步长 step，patch 边长 P，求每块增量并摊回像素
static void patch_pass(const Img &a, const Img &b, std::vector<float> &u, std::vector<float> &v,
                       int P, int step, float maxstep) {
    const int w = a.w, h = a.h;
    std::vector<float> su((size_t)w * h, 0.f), sv((size_t)w * h, 0.f), wt((size_t)w * h, 0.f);
    const int half = P / 2;
    for (int cy = half; cy < h - half; cy += step) {
        for (int cx = half; cx < w - half; cx += step) {
            const float u0 = u[(size_t)cy * w + cx], v0 = v[(size_t)cy * w + cx];
            float gxx = 0, gxy = 0, gyy = 0, bx = 0, by = 0;
            int n = 0;
            for (int y = std::max(1, cy - half); y < cy + half - 1; y++) {
                const uint8_t *row = a.row(y);
                for (int x = std::max(1, cx - half); x < cx + half - 1; x++) {
                    float gx = 0.5f * (row[x + 1] - row[x - 1]);
                    float gy = 0.5f * (a.row(y + 1)[x] - a.row(y - 1)[x]);
                    float it = bilin(b, x + u0, y + v0) - (float)row[x];
                    gxx += gx * gx; gxy += gx * gy; gyy += gy * gy;
                    bx += gx * it; by += gy * it;
                    n++;
                }
            }
            if (n < 24) continue;
            float det = gxx * gyy - gxy * gxy;
            if (det < 1e-2f * (float)n) continue;   // 平坦块：梯度退化，跳过
            float du = -(gyy * bx - gxy * by) / det;
            float dv = -(gxx * by - gxy * bx) / det;
            du = std::max(-maxstep, std::min(maxstep, du));
            dv = std::max(-maxstep, std::min(maxstep, dv));
            if (!(du == du) || !(dv == dv)) continue;
            for (int y = cy - half; y < cy + half; y++)
                for (int x = cx - half; x < cx + half; x++) {
                    size_t i = (size_t)y * w + x;
                    su[i] += du; sv[i] += dv; wt[i] += 1.f;
                }
        }
    }
    for (size_t i = 0; i < su.size(); i++) {
        if (wt[i] > 0.f) { u[i] += su[i] / wt[i]; v[i] += sv[i] / wt[i]; }
    }
    tri_smooth(u, w, h);
    tri_smooth(v, w, h);
}

static void flow_dis(const Img &A, const Img &B, std::vector<Vec2> &out) {
    std::vector<Img> pa, pb;
    pa.push_back(A); pb.push_back(B);
    while ((int)pa.size() < 6 && std::min(pa.back().w, pa.back().h) > 48) {
        Img na, nb;
        down2(pa.back(), na); down2(pb.back(), nb);
        pa.push_back(na); pb.push_back(nb);
    }
    const int L = (int)pa.size();
    int pw = 0, ph = 0;
    std::vector<float> u, v;
    for (int l = L - 1; l >= 0; l--) {
        const Img &a = pa[l], &b = pb[l];
        if (pw == 0) { u.assign((size_t)a.w * a.h, 0.f); v.assign((size_t)a.w * a.h, 0.f); }
        else { std::vector<float> nu, nv; upf(u, pw, ph, nu, a.w, a.h); upf(v, pw, ph, nv, a.w, a.h); u.swap(nu); v.swap(nv); }
        // 粗层大步长、细层小步长；patch 尺寸随层收缩，两次错位网格保证覆盖
        fprintf(stderr, "dis lvl=%d %dx%d\n", l, a.w, a.h);
        int P = (l == L - 1) ? 16 : (l == L - 2 ? 12 : 8);
        patch_pass(a, b, u, v, P, P / 2, g_dms);
        patch_pass(a, b, u, v, P, std::max(2, P / 4), g_dms * 0.75f);
        patch_pass(a, b, u, v, P, std::max(2, P / 4), 1.0f);
        for (int it = 0; it < (l == 0 ? g_dit : 3); it++)
            flow_level(a, b, u, v, 2, (l == 0) ? g_da : 1.2f);
        for (int e = 0; e < (l == 0 ? g_dp : 0); e++)
            patch_pass(a, b, u, v, 8, 2, 0.5f);
        pw = a.w; ph = a.h;
    }
    out.resize((size_t)A.w * A.h);
    for (size_t i = 0; i < out.size(); i++) out[i] = Vec2{u[i], v[i]};
}

// 前后向一致性：|F(p) + G(p+F(p))| 大于阈值判为遮挡
static void fb_occlusion(const std::vector<Vec2> &F, const std::vector<Vec2> &G,
                         int w, int h, float thr, std::vector<uint8_t> &occ) {
    occ.assign((size_t)w * h, 0);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            float gx = x + F[i].x, gy = y + F[i].y;
            if (gx < 0 || gy < 0 || gx > w - 1.001f || gy > h - 1.001f) { occ[i] = 1; continue; }
            int x0 = (int)gx, y0 = (int)gy;
            int x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
            float ax = gx - x0, ay = gy - y0;
            Vec2 q00 = G[(size_t)y0 * w + x0], q01 = G[(size_t)y0 * w + x1];
            Vec2 q10 = G[(size_t)y1 * w + x0], q11 = G[(size_t)y1 * w + x1];
            float bx = (q00.x * (1 - ax) + q01.x * ax) * (1 - ay) + (q10.x * (1 - ax) + q11.x * ax) * ay;
            float by = (q00.y * (1 - ax) + q01.y * ax) * (1 - ay) + (q10.y * (1 - ax) + q11.y * ax) * ay;
            float ex = F[i].x + bx, ey = F[i].y + by;
            occ[i] = (sqrtf(ex * ex + ey * ey) > thr) ? 1 : 0;
        }
    }
}
int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: alhift in.yuv W H N out.yuv [mult]\n");
        return 2;
    }
    const char *inp = argv[1];
    W = atoi(argv[2]); H = atoi(argv[3]);
    int NFR = atoi(argv[4]);
    const char *outp = argv[5];
    float mult = (argc > 6) ? (float)atof(argv[6]) : 2.0f;
    int opt = (argc > 7) ? atoi(argv[7]) : 3;
    if (W <= 0 || H <= 0 || NFR <= 0 || mult < 1.01f) return 2;
    bool penFixed = false;
    if (const char *e = getenv("ALH_PEN")) { g_pen = (float)atof(e); penFixed = true; }
    if (const char *e = getenv("ALH_WIN")) g_win = atoi(e);
    if (const char *e = getenv("ALH_FIT")) g_fit = atoi(e);
    if (const char *e = getenv("ALH_FAL")) g_fal = (float)atof(e);
    if (const char *e = getenv("ALH_VMED")) g_vmed = atoi(e);
    if (const char *e = getenv("ALH_DA")) g_da = (float)atof(e);
    if (const char *e = getenv("ALH_DIT")) g_dit = atoi(e);
    if (const char *e = getenv("ALH_DP")) g_dp = atoi(e);
    if (const char *e = getenv("ALH_DMS")) g_dms = (float)atof(e);
    if (const char *e = getenv("ALH_DUP")) g_dupThr = (float)atof(e);
    if (const char *e = getenv("ALH_OFRES")) g_ofres = atoi(e);
    if (const char *e = getenv("ALH_HSOUT")) g_hsout = atoi(e);
    if (const char *e = getenv("ALH_HSIN")) g_hsin = atoi(e);
    if (const char *e = getenv("ALH_HSA")) g_hsa = (float)atof(e);
    if (const char *e = getenv("ALH_HSM")) g_hsm = atoi(e);
    size_t ysz = (size_t)W * H, csz = (size_t)(W / 2) * (H / 2);
    size_t fsz = ysz + 2 * csz;

    FILE *fi = (strcmp(inp, "-") == 0) ? stdin : fopen(inp, "rb");
    FILE *fo = (strcmp(outp, "-") == 0) ? stdout : fopen(outp, "wb");
    if (!fi || !fo) { fprintf(stderr, "open failed\n"); return 3; }

    if (NFR < 2) { fprintf(stderr, "need >=2 frames (got %d)\n", NFR); return 2; }
    vector<uint8_t> fa(fsz), fb(fsz);
    if (fread(fa.data(), 1, fsz, fi) != fsz) { fprintf(stderr, "need >=2 frames\n"); return 3; }

    Img a, b;
    a.alloc(W, H); b.alloc(W, H);

    // 金字塔层数
    LEVELS = 1;
    for (int s = std::min(W, H); s >= 40; s /= 2) LEVELS++;
    LEVELS = std::min(LEVELS, 4);

    vector<Img> pa(LEVELS), pb(LEVELS);
    vector<int> bws(LEVELS), bhs(LEVELS);

    int done = 0;
    int ndup = 0;
    bool dumped = false;   // 自检 dump 只写首对帧，循环外置
    for (int k = 0; k < NFR; k++) {
        if (k > 0) {
            if (k + 1 < NFR) {
                if (fread(fb.data(), 1, fsz, fi) != fsz) break;
            } else {
                fb = fa;   // 尾帧复制
            }
        } else {
            if (fread(fb.data(), 1, fsz, fi) != fsz) break;
        }
        memcpy(a.p.data(), fa.data(), ysz);
        memcpy(b.p.data(), fb.data(), ysz);

        // 首次拿到真实像素后估噪声：干净片贴合 ffmpeg 的 0.25，噪声越大越松
        // （噪声让 SAD 不可靠，重罚会把矢量场压回零矢量，画面反而糊）
        if (!penFixed) {
            float sg = y_sigma(a);
            g_pen = 0.25f * (1.0f - (sg - 1.2f) / 3.3f);
            if (g_pen < 0.02f) g_pen = 0.02f;
            if (g_pen > 0.25f) g_pen = 0.25f;
            penFixed = true;
            // 去重阈值跟着噪声走：干净片 0.8 够用；颗粒重的片子必须放宽，
            // 否则静止镜头的帧间差被噪声顶起来，永远判不出重复。
            // 实测：纯噪声静止画面的帧间 MAD ≈ 1.5σ，真实运动画面 ≥ 3σ，
            // 所以阈值取 1.6σ 能把"颗粒噪点"判成静止，同时不误吃真实运动。
            if (g_dupThr < 0.f) {
                g_dupThr = 1.6f * sg;
                if (g_dupThr < 0.8f) g_dupThr = 0.8f;
                if (g_dupThr > 8.0f) g_dupThr = 8.0f;
            }
            fprintf(stderr, "sigma=%.2f pen=%.3f dup=%.2f\n", sg, g_pen, g_dupThr);
        }

        // 这一段要输出的帧数：按累积相位取整，支持非整数倍（24fps→60fps 的 2.5 倍）
        const int seg0 = (int)llround((double)k * mult);
        const int seg1 = (int)llround((double)(k + 1) * mult);
        const int nseg = seg1 - seg0;

        // 去重复帧：前后两帧几乎一致时直接复制，不插值（避免鬼影与假运动）
        if (opt & 4096) {
            const uint8_t *pa_ = fa.data(), *pb_ = fb.data();
            long dsum = 0;
            for (size_t i = 0; i < ysz; i += 4) dsum += std::abs((int)pa_[i] - (int)pb_[i]);
            float mad = (float)dsum / (float)(ysz / 4);
            if (mad < g_dupThr) {
                for (int s = 0; s < nseg; s++) fwrite(fa.data(), 1, fsz, fo);
                done += nseg;
                ndup++;
                fprintf(stderr, "k=%d dup mad=%.2f thr=%.2f\n", k, mad, g_dupThr);
                continue;
            }
        }

        // 场景切换：两帧几乎无相关性时任何插值都只会糊出鬼影，直接复制当前帧
        if (opt & 262144) {
            long dsum = 0; int cnt = 0;
            for (int y = 0; y < H; y += 4) {
                const uint8_t *ra = fa.data() + (size_t)y * W, *rb = fb.data() + (size_t)y * W;
                for (int x = 0; x < W; x += 4) { dsum += std::abs((int)ra[x] - (int)rb[x]); cnt++; }
            }
            if (cnt > 0 && (float)dsum / cnt > 26.0f) {
                for (int q = 0; q < nseg; q++) fwrite(fa.data(), 1, fsz, fo);
                done += nseg;
                fprintf(stderr, "k=%d scd\n", k);
                fa.swap(fb);
                continue;
            }
        }

        fprintf(stderr, "k=%d pyr\n", k);
        // 金字塔
        pa[0] = a; pb[0] = b;
        if (opt & 8192) { blur3(pa[0]); blur3(pb[0]); }
        for (int l = 1; l < LEVELS; l++) { down2(pa[l - 1], pa[l]); down2(pb[l - 1], pb[l]); }
        // 粗到细块匹配（可双向：A→B 与 B→A）
        auto mkfield = [&](vector<Img> &A, vector<Img> &Bs, vector<Vec2> &out, int &ow, int &oh) {
            vector<Vec2> c2; int cw = 0, ch = 0;
            for (int l = LEVELS - 1; l >= 0; l--) {
                int Bb = 8;
                int rad = (l == LEVELS - 1) ? 8 : 2;
                if (l == 0) rad = (opt & 134217728) ? 16 : ((opt & 67108864) ? 12 : ((opt & 33554432) ? 8 : ((opt & 32) ? 6 : 3)));
                int tw = (A[l].w + Bb - 1) / Bb, th = (A[l].h + Bb - 1) / Bb;
                vector<Vec2> init;
                if (c2.empty()) init.assign((size_t)tw * th, Vec2{0, 0});
                else init = upsample(c2, cw, ch, tw, th);
                vector<Vec2> res; vector<float> cost; int rw = 0, rh = 0;
                match_level(A[l], Bs[l], Bb, rad, init, res, cost, rw, rh);
                c2 = res; cw = rw; ch = rh;
            }
            median3(c2, cw, ch);
            out = c2; ow = cw; oh = ch;
        };
        int nbw = 0, nbh = 0;
        vector<Vec2> cur;
        vector<Vec2> gfield; int gw = 0, gh = 0;
        if (opt & 4194304) {
            // 低分辨率逐像素光流：整幅场在 ~250px 上解，再放大回全分辨率
            flow_gn_lowres(pa[0], pb[0], cur);
            flow_gn_lowres(pb[0], pa[0], gfield);
            nbw = W; nbh = H; gw = W; gh = H;
            if (opt & 268435456) {
                vector<Vec2> blk, blkb; int bkw = 0, bkh = 0, bkw2 = 0, bkh2 = 0;
                mkfield(pa, pb, blk, bkw, bkh);
                mkfield(pb, pa, blkb, bkw2, bkh2);
                blend_candidates(pa[0], pb[0], W, H, cur, blk, bkw, bkh, 8);
                blend_candidates(pb[0], pa[0], W, H, gfield, blkb, bkw2, bkh2, 8);
            }
        } else if (opt & 1073741824) {
            // DIS 式稠密光流：多尺度 patch 逆向搜索 + 每层变分细化，输出逐像素连续场
            cur.assign((size_t)W * H, Vec2{0.f, 0.f});
            gfield.assign((size_t)W * H, Vec2{0.f, 0.f});
            flow_dis(pa[0], pb[0], cur);
            flow_dis(pb[0], pa[0], gfield);
            // 自检用：把首对帧的逐像素场原样导出（float u,v 交错，行优先）
            if (!dumped) {
                if (const char *dp = getenv("ALH_FLOWDUMP")) {
                    FILE *dfp = fopen(dp, "wb");
                    if (dfp) {
                        fwrite(cur.data(), sizeof(Vec2), (size_t)W * H, dfp);
                        fclose(dfp);
                        fprintf(stderr, "flowdump %dx%d\n", W, H);
                    }
                }
                dumped = true;
            }
            nbw = W; nbh = H; gw = W; gh = H;
        } else if (opt & 8388608) {
            // 稠密光流路线：块匹配给大位移种子，稠密迭代把场拉连续、补到亚像素
            mkfield(pa, pb, cur, nbw, nbh);
            mkfield(pb, pa, gfield, gw, gh);
            {
                // 逐像素场：块场只当低分辨率种子，全分辨率靠光流迭代铺开。
                // 这一档不在全分辨率上写整块矢量——整块写回就是画面上那些方格。
                vector<Vec2> dense;
                flow_dense(pa, pb, W, H, dense, nullptr, 0, 0, 8);
                if (opt & 67108864) {
                    // 双向一致性加权的混合场：光流在纹理丰富处准，块场在大位移处稳。
                    // 前向流 F 与反向流 G 应当互为逆，|F(p)+G(p+F(p))| 小说明这一点可信，
                    // 用这个残差做权重；两场都是连续的，混完仍然没有方格。
                    const float TAU = 2.2f, IT = 1.0f / (2.0f * TAU * TAU);
                    for (int y = 0; y < H; y++) {
                        for (int x = 0; x < W; x++) {
                            size_t i = (size_t)y * W + x;
                            Vec2 f = dense[i];
                            Vec2 g = sample_vec_b(gfield, gw, gh, 8, x + f.x, y + f.y);
                            float rx = f.x + g.x, ry = f.y + g.y;
                            float c = expf(-(rx * rx + ry * ry) * IT);
                            Vec2 bs = sample_vec_b(cur, nbw, nbh, 8, (float)x, (float)y);
                            dense[i] = Vec2{c * f.x + (1 - c) * bs.x, c * f.y + (1 - c) * bs.y};
                        }
                    }
                }
                cur.swap(dense);
            }
            nbw = W; nbh = H;
            fprintf(stderr, "k=%d flow dense\n", k);
        } else {
            mkfield(pa, pb, cur, nbw, nbh);
            // 反向场始终要算：合成阶段用它做双向一致性校验，遮挡区才不会糊成重影
            mkfield(pb, pa, gfield, gw, gh);
        }
        // opt&8: 在最细层再做一遍 4×4 块精修（矢量场加密，减少块状接缝）
        if (opt & 8) {
            int B2 = 4;
            int tw = (pa[0].w + B2 - 1) / B2, th = (pa[0].h + B2 - 1) / B2;
            vector<Vec2> init = upsample(cur, nbw, nbh, tw, th);
            vector<Vec2> res; vector<float> cost; int rw = 0, rh = 0;
            match_level(pa[0], pb[0], B2, 2, init, res, cost, rw, rh);
            cur = res; nbw = rw; nbh = rh;
        }
        fprintf(stderr, "k=%d match done %dx%d\n", k, nbw, nbh);
        int B = (opt & (8388608 | 1073741824 | 4194304)) ? 1 : ((opt & 8) ? 4 : 8), bw = nbw, bh = nbh;
        if (B > 1) median3(cur, bw, bh);
        if (opt & 16) { median3(cur, bw, bh); median3(cur, bw, bh); }   // 多遍中值：压块状拼布
        if (B > 1) {
            if (opt & 1) refine_sub(pa[0], pb[0], cur, B, bw, bh);
            if (opt & 524288) refine_lk(pa[0], pb[0], cur, B, bw, bh, 3);
            if (opt & 1048576) refine_dense(pa[0], pb[0], cur, B, bw, bh, 3);
        }
        if (opt & 1) median3(cur, bw, bh);
        // 时间一致性：与上一对帧的矢量场轻微融合，压掉逐帧跳变
        static vector<Vec2> prevV;
        if ((opt & 2) && prevV.size() == cur.size()) {
            for (size_t i = 0; i < cur.size(); i++) {
                cur[i].x = 0.65f * cur[i].x + 0.35f * prevV[i].x;
                cur[i].y = 0.65f * cur[i].y + 0.35f * prevV[i].y;
            }
        }
        prevV = cur;
        // 残差建档，用于置信度
        vector<float> conf((size_t)bw * bh, 1.0f);
        for (int by = 0; by < bh; by++) {
            for (int bx = 0; bx < bw; bx++) {
                int px = bx * B, py = by * B;
                int x1 = std::min(px + B, W), y1 = std::min(py + B, H);
                Vec2 v = cur[(size_t)by * bw + bx];
                long sad = 0; int cnt = 0;
                for (int y = py; y < y1; y++) {
                    for (int x = px; x < x1; x++) {
                        float s = bilin(b, x + v.x, y + v.y);
                        sad += (long)fabsf((float)a.row(y)[x] - s);
                        cnt++;
                    }
                }
                float m = cnt ? (float)sad / cnt : 0;
                conf[(size_t)by * bw + bx] = 1.0f / (1.0f + m * m / 900.0f);
            }
        }

        fprintf(stderr, "k=%d conf done\n", k);
        // 稠密光流档：用前后向一致性标出遮挡像素，合成时那里只取一侧，避免双影
        std::vector<uint8_t> occ;
        if (opt & 1073741824) fb_occlusion(cur, gfield, W, H, 1.5f, occ);
        const int hw = W / 2, hh = H / 2;
        const float SIG2 = 2.0f * 40.0f * 40.0f;
        // 输出本区间所有帧：t 按真实相位算，非整数倍也成立，帧数严格等于 round(N×倍率)。
        for (int gi = seg0; gi < seg1; gi++) {
            float t = (float)(((double)gi - (double)k * mult) / mult);
            if (t < 1e-4f) { fwrite(fa.data(), 1, fsz, fo); done++; continue; }
            vector<uint8_t> out(fsz);
            uint8_t *oy = out.data(), *ou = out.data() + ysz, *ov = ou + csz;
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    float P0 = 0.0f, P1 = 0.0f, wsum = 0.0f, w0s = 0.0f, w1s = 0.0f;
                    if (opt & 32768) {
                        // OBMC：每个像素用周围 4 个块的矢量各 w 一次，按到块中心的距离加权合并。
                        // 块与块的接缝因此被抹平，运动边缘不再出现方格子。
                        int bx0 = std::min(bw - 1, x / B), by0 = std::min(bh - 1, y / B);
                        int sgnx = (x >= bx0 * B + B / 2) ? 1 : -1;
                        int sgny = (y >= by0 * B + B / 2) ? 1 : -1;
                        int cxb[2] = {bx0, bx0 + sgnx}, cyb[2] = {by0, by0 + sgny};
                        for (int j = 0; j < 2; j++) {
                            for (int i = 0; i < 2; i++) {
                                int cx = std::min(bw - 1, std::max(0, cxb[i]));
                                int cy = std::min(bh - 1, std::max(0, cyb[j]));
                                float ccx = cx * B + B * 0.5f, ccy = cy * B + B * 0.5f;
                                float wk = (1.0f - fabsf((float)x - ccx) / B) * (1.0f - fabsf((float)y - ccy) / B);
                                if (wk <= 0.02f) continue;
                                Vec2 v = cur[(size_t)cy * bw + cx];
                                float ax = x - v.x * t, ay = y - v.y * t;
                                float bxp = x + v.x * (1 - t), byp = y + v.y * (1 - t);
                                if (ax < 0 || ay < 0 || ax > W - 1 || ay > H - 1) continue;
                                if (bxp < 0 || byp < 0 || bxp > W - 1 || byp > H - 1) continue;
                                // 反向场一致性：这一侧的矢量走回来对不上，说明该点被遮挡或在做
                                // 非刚性运动，降低 b 侧权重，避免把 b 的错位内容糊进来（重影/果冻）
                                float trust = 1.0f;
                                if ((opt & 65536) && gw > 0) {
                                    Vec2 gg = sample_vec_b(gfield, gw, gh, B, std::min(W - 1, std::max(0, (int)lroundf(x + v.x))), std::min(H - 1, std::max(0, (int)lroundf(y + v.y))));
                                    float ex = v.x + gg.x, ey = v.y + gg.y;
                                    float rr = sqrtf(ex * ex + ey * ey);
                                    trust = 1.0f / (1.0f + rr * rr * 0.25f);
                                }
                                P0 += bilin(a, ax, ay) * wk;
                                P1 += bilin(b, bxp, byp) * wk * trust;
                                wsum += wk;
                                w0s += wk;
                                w1s += wk * trust;
                            }
                        }
                        if (wsum > 1e-4f) {
                            P0 /= (w0s > 1e-4f ? w0s : wsum);
                            P1 /= (w1s > 1e-4f ? w1s : wsum);
                        } else { wsum = 0.0f; }
                    } else {
                        Vec2 v = (opt & 256) ? sample_vec_b(cur, bw, bh, B, x, y) : sample_vech(cur, bw, bh, B, x, y);
                        float ax = x - v.x * t, ay = y - v.y * t;
                        float bxp = x + v.x * (1 - t), byp = y + v.y * (1 - t);
                        bool i0 = ax >= 0 && ay >= 0 && ax <= W - 1 && ay <= H - 1;
                        bool i1 = bxp >= 0 && byp >= 0 && bxp <= W - 1 && byp <= H - 1;
                        if (i0 && i1) { P0 = bilin(a, ax, ay); P1 = bilin(b, bxp, byp); wsum = 1.0f; }
                        else if (i0) { P0 = bilin(a, ax, ay); P1 = P0; wsum = 1.0f; }
                        else if (i1) { P1 = bilin(b, bxp, byp); P0 = P1; wsum = 1.0f; }
                    }
                    // 反向场当第二组候选：F 与 G 对同一中间点给出两个版本，
                    // 取 warp 残差小的那个，单向遮挡处就不会被糊成双影
                    if ((opt & 131072) && gw > 0 && wsum > 1e-4f && fabsf(P0 - P1) > 12.0f) {
                        Vec2 gg = sample_vec_b(gfield, gw, gh, B, x, y);
                        float ax2 = x + gg.x * t, ay2 = y + gg.y * t;
                        float bx2 = x - gg.x * (1 - t), by2 = y - gg.y * (1 - t);
                        if (ax2 >= 0 && ay2 >= 0 && ax2 <= W - 1 && ay2 <= H - 1 &&
                            bx2 >= 0 && by2 >= 0 && bx2 <= W - 1 && by2 <= H - 1) {
                            float q0 = bilin(a, ax2, ay2), q1 = bilin(b, bx2, by2);
                            float d1 = fabsf(P0 - P1), d2 = fabsf(q0 - q1);
                            if (d2 < d1) { P0 = q0; P1 = q1; }
                        }
                    }
                    float tm = bilin(a, (float)x, (float)y) * (1 - t) + bilin(b, (float)x, (float)y) * t;
                    float val;
                    if (wsum <= 1e-4f) {
                        val = tm;
                    } else if (opt & 2097152) {
                        // Super SloMo 式可见性加权：两侧各自的置信度必须分开保留，
                        // 否则归一化会把置信度整体约掉（老 bug）。V0/V1 用「warp 回原位置是否还对得上」来量。
                        float vx = 0, vy = 0, e0 = 0, e1 = 0;
                        {
                            Vec2 vv = sample_vec_b(cur, bw, bh, B, x, y);
                            vx = vv.x; vy = vv.y;
                            float axx = x - vx * t, ayy = y - vy * t;
                            float bxx = x + vx * (1 - t), byy = y + vy * (1 - t);
                            e0 = fabsf(bilin(a, axx, ayy) - bilin(b, axx + vx, ayy + vy));
                            e1 = fabsf(bilin(b, bxx, byy) - bilin(a, bxx - vx, byy - vy));
                        }
                        float V0 = expf(-e0 * e0 / SIG2), V1 = expf(-e1 * e1 / SIG2);
                        float a0 = (1 - t) * V0, a1 = t * V1, Z = a0 + a1;
                        val = (Z < 0.05f) ? tm : (P0 * a0 + P1 * a1) / Z;
                    } else {
                        float mx = P0 * (1 - t) + P1 * t;
                        if (opt & 16384) {
                            // 两侧 warp 的差就是「这一点到底能不能信矢量」的度量。
                            // 差得越大越可能是遮挡或错矢量，连续地退回同时刻平均（不硬切换，才不闪）。
                            float d = fabsf(P0 - P1);
                            float cpx = expf(-(d * d) / SIG2);
                            if (cpx < 0.0f) cpx = 0.0f;
                            val = mx * cpx + tm * (1 - cpx);
                        } else {
                            val = mx;
                        }
                    }
                    // 遮挡区只取一侧：被遮住的那一侧本来就看不到，混进来就是双影
                    if (!occ.empty() && occ[(size_t)y * W + x]) val = (t < 0.5f) ? P0 : P1;
                    oy[(size_t)y * W + x] = (uint8_t)std::max(0, std::min(255, (int)lroundf(val)));
                }
            }
            // 色度：同样按矢量场 warp（U/V 两个平面一起算）
            for (int y = 0; y < hh; y++) {
                for (int x = 0; x < hw; x++) {
                    const uint8_t *au = fa.data() + ysz, *av = au + csz;
                    const uint8_t *bu = fb.data() + ysz, *bv = bu + csz;
                    auto smp = [&](const uint8_t *pl, float sx, float sy) -> float {
                        sx = std::max(0.0f, std::min((float)hw - 1.001f, sx));
                        sy = std::max(0.0f, std::min((float)hh - 1.001f, sy));
                        int ix = (int)floorf(sx), iy = (int)floorf(sy);
                        int jx = std::min(hw - 1, ix + 1), jy = std::min(hh - 1, iy + 1);
                        float fx = sx - ix, fy = sy - iy;
                        float r0 = pl[(size_t)iy * hw + ix] * (1 - fx) + pl[(size_t)iy * hw + jx] * fx;
                        float r1 = pl[(size_t)jy * hw + ix] * (1 - fx) + pl[(size_t)jy * hw + jx] * fx;
                        return r0 * (1 - fy) + r1 * fy;
                    };
                    float u0 = 0, u1 = 0, v0 = 0, v1 = 0, ws = 0;
                    auto addTap = [&](Vec2 v, float wk) {
                        float sx = x - v.x * t * 0.5f, sy = y - v.y * t * 0.5f;
                        float sx2 = x + v.x * (1 - t) * 0.5f, sy2 = y + v.y * (1 - t) * 0.5f;
                        u0 += smp(au, sx, sy) * wk; v0 += smp(av, sx, sy) * wk;
                        u1 += smp(bu, sx2, sy2) * wk; v1 += smp(bv, sx2, sy2) * wk;
                        ws += wk;
                    };
                    if (opt & 32768) {
                        int bx0 = std::min(bw - 1, (x * 2) / B), by0 = std::min(bh - 1, (y * 2) / B);
                        int sgnx = (x * 2 >= bx0 * B + B / 2) ? 1 : -1;
                        int sgny = (y * 2 >= by0 * B + B / 2) ? 1 : -1;
                        for (int j = 0; j < 2; j++) {
                            for (int i = 0; i < 2; i++) {
                                int cx = std::min(bw - 1, std::max(0, bx0 + (i ? sgnx : 0)));
                                int cy = std::min(bh - 1, std::max(0, by0 + (j ? sgny : 0)));
                                float ccx = (cx * B + B * 0.5f) * 0.5f, ccy = (cy * B + B * 0.5f) * 0.5f;
                                float wk = (1.0f - fabsf((float)x - ccx) / (B * 0.5f)) * (1.0f - fabsf((float)y - ccy) / (B * 0.5f));
                                if (wk <= 0.02f) continue;
                                addTap(cur[(size_t)cy * bw + cx], wk);
                            }
                        }
                        if (ws > 1e-4f) { u0 /= ws; u1 /= ws; v0 /= ws; v1 /= ws; }
                    } else {
                        Vec2 v = (opt & 256) ? sample_vec_b(cur, bw, bh, B, x * 2, y * 2) : sample_vech(cur, bw, bh, B, x * 2, y * 2);
                        addTap(v, 1.0f);
                    }
                    ou[(size_t)y * hw + x] = (uint8_t)std::max(0, std::min(255, (int)lroundf(u0 * (1 - t) + u1 * t)));
                    ov[(size_t)y * hw + x] = (uint8_t)std::max(0, std::min(255, (int)lroundf(v0 * (1 - t) + v1 * t)));
                }
            }
            fwrite(out.data(), 1, fsz, fo);
            done++;
        }
        fa.swap(fb);
    }
    fclose(fi); fclose(fo);
    if (done == 0) { fprintf(stderr, "no frames written\n"); return 4; }
    fprintf(stderr, "wrote %d frames dup=%d\n", done, ndup);
    return 0;
}