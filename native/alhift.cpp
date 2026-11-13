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
            fprintf(stderr, "sigma=%.2f pen=%.3f\n", sg, g_pen);
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
            if (mad < 0.8f) {
                for (int s = 0; s < nseg; s++) fwrite(fa.data(), 1, fsz, fo);
                done += nseg;
                ndup++;
                fprintf(stderr, "k=%d dup mad=%.2f\n", k, mad);
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
        mkfield(pa, pb, cur, nbw, nbh);
        vector<Vec2> gfield; int gw = 0, gh = 0;
        // 反向场始终要算：合成阶段用它做双向一致性校验，遮盖/遮挡区才不会糊成重影
        mkfield(pb, pa, gfield, gw, gh);
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
        int B = (opt & 8) ? 4 : 8, bw = nbw, bh = nbh;
        median3(cur, bw, bh);
        if (opt & 16) { median3(cur, bw, bh); median3(cur, bw, bh); }   // 多遍中值：压块状拼布
        if (opt & 1) refine_sub(pa[0], pb[0], cur, B, bw, bh);
        if (opt & 524288) refine_lk(pa[0], pb[0], cur, B, bw, bh, 3);
        if (opt & 1048576) refine_dense(pa[0], pb[0], cur, B, bw, bh, 3);
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