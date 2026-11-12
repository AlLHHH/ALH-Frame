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
                            // 边界：整块必须落在 b 内（边缘块按可用区域算）
                            int x0 = px, y0_ = py;
                            int x1 = std::min(px + B, a.w), y1_ = std::min(py + B, a.h);
                            // 源块四角都必须落在 b 内，否则跳过（越界读会崩）
                            if (px + sx < 0 || py + sy < 0 || x1 + sx > b.w || y1_ + sy > b.h) continue;
                            long sad = 0;
                            for (int y = y0_; y < y1_; y++) {
                                const uint8_t *ra = a.row(y) + x0;
                                const uint8_t *rb = b.row(y + sy) + x0 + sx;
                                for (int x = 0; x < x1 - x0; x++) {
                                    int d = (int)ra[x] - (int)rb[x];
                                    sad += d < 0 ? -d : d;
                                }
                            }
                            // 轻微惩罚大矢量，抑制纹理重复导致的跳变
                            float c = (float)sad + 0.02f * (fabsf((float)sx) + fabsf((float)sy));
                            if (c < best) { best = c; bdx = sx; bdy = sy; }
                        }
                    }
                    if (pass == 0 && bdx == 0 && bdy == 0) break;
                    cx = bdx; cy = bdy;
                }
                out[(size_t)by * nx + bx] = Vec2{(float)bdx, (float)bdy};
                cost[(size_t)by * nx + bx] = best / (float)(B * B);
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
                float c0 = sad_at(a, b, px, py, B, t.x, t.y);
                float cx1 = sad_at(a, b, px, py, B, t.x + 0.5f, t.y);
                float cx0 = sad_at(a, b, px, py, B, t.x - 0.5f, t.y);
                float cy1 = sad_at(a, b, px, py, B, t.x, t.y + 0.5f);
                float cy0 = sad_at(a, b, px, py, B, t.x, t.y - 0.5f);
                auto parab = [&](float m1, float c, float p1) -> float {
                    float den = m1 - 2 * c + p1;
                    if (fabsf(den) < 1e-3f) return 0;
                    float d = 0.5f * (m1 - p1) / den;
                    return std::max(-0.5f, std::min(0.5f, d));
                };
                t.x += parab(cx0, c0, cx1) * 0.5f;
                t.y += parab(cy0, c0, cy1) * 0.5f;
                v[(size_t)by * bw + bx] = t;
            }
        }
    };
    vector<std::thread> th;
    int step = std::max(1, bh / nthreads);
    for (int y = 0; y < bh; y += step) th.emplace_back(work, y, std::min(bh, y + step));
    for (auto &t : th) t.join();
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
    int mult = (argc > 6) ? atoi(argv[6]) : 2;
    int opt = (argc > 7) ? atoi(argv[7]) : 3;
    if (W <= 0 || H <= 0 || NFR <= 0 || mult < 2) return 2;
    size_t ysz = (size_t)W * H, csz = (size_t)(W / 2) * (H / 2);
    size_t fsz = ysz + 2 * csz;

    FILE *fi = (strcmp(inp, "-") == 0) ? stdin : fopen(inp, "rb");
    FILE *fo = (strcmp(outp, "-") == 0) ? stdout : fopen(outp, "wb");
    if (!fi || !fo) { fprintf(stderr, "open failed\n"); return 3; }

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

        // 去重复帧：前后两帧几乎一致时直接复制，不插值（避免鬼影与假运动）
        if (opt & 4096) {
            const uint8_t *pa_ = fa.data(), *pb_ = fb.data();
            long dsum = 0;
            for (size_t i = 0; i < ysz; i += 4) dsum += std::abs((int)pa_[i] - (int)pb_[i]);
            float mad = (float)dsum / (float)(ysz / 4);
            if (mad < 0.8f) {
                for (int s = 0; s < mult; s++) fwrite(fa.data(), 1, fsz, fo);
                done += mult;
                ndup++;
                fprintf(stderr, "k=%d dup mad=%.2f\n", k, mad);
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
                if (l == 0) rad = 3;
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
        // 输出：原始帧 + 插值帧（mult-1 张）
        fwrite(fa.data(), 1, fsz, fo);
        done++;
        for (int s = 1; s < mult; s++) {
            float t = (float)s / (float)mult;
            vector<uint8_t> out(fsz);
            uint8_t *oy = out.data(), *ou = out.data() + ysz, *ov = ou + csz;
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    Vec2 v = (opt & 256) ? sample_vec_b(cur, bw, bh, B, x, y) : sample_vech(cur, bw, bh, B, x, y);
                    float c = conf[(size_t)(y / B) * bw + (x / B)];
                    if (opt & 4) c = 1.0f;
                    // 多假设：取像素四周 4 个块的矢量作候选，按双向一致性 WTA 选一个（OBMC 式）
                    if (opt & 512) {
                        int bx0 = x / B, by0 = y / B;
                        int oxs[4] = {bx0 - 1, bx0, bx0 - 1, bx0};
                        int oys[4] = {by0 - 1, by0 - 1, by0, by0};
                        float best = 1e9f, bp0 = 0, bp1 = 0;
                        Vec2 bv = v;
                        for (int k = 0; k < 4; k++) {
                            int cx = std::min(bw - 1, std::max(0, oxs[k]));
                            int cy = std::min(bh - 1, std::max(0, oys[k]));
                            Vec2 cd = cur[(size_t)cy * bw + cx];
                            float xx0 = x - cd.x * t, yy0 = y - cd.y * t;
                            float xx1 = x + cd.x * (1 - t), yy1 = y + cd.y * (1 - t);
                            if (xx0 < 0 || yy0 < 0 || xx0 > W - 1 || yy0 > H - 1) continue;
                            if (xx1 < 0 || yy1 < 0 || xx1 > W - 1 || yy1 > H - 1) continue;
                            float q0 = bilin(a, xx0, yy0), q1 = bilin(b, xx1, yy1);
                            float d = fabsf(q0 - q1);
                            if (d < best) { best = d; bp0 = q0; bp1 = q1; bv = cd; }
                        }
                        if (best < 1e8f) {
                            float w0 = (1 - t) * (0.35f + 0.65f * c), w1 = t * (0.35f + 0.65f * c);
                            float val = (bp0 * w0 + bp1 * w1) / (w0 + w1);
                            oy[(size_t)y * W + x] = (uint8_t)std::max(0, std::min(255, (int)lroundf(val)));
                            continue;
                        }
                    }
                    float x0 = x - v.x * t, y0 = y - v.y * t;
                    float x1 = x + v.x * (1 - t), y1 = y + v.y * (1 - t);
                    bool in0 = x0 >= 0 && y0 >= 0 && x0 <= W - 1 && y0 <= H - 1;
                    bool in1 = x1 >= 0 && y1 >= 0 && x1 <= W - 1 && y1 <= H - 1;
                    float p0 = in0 ? bilin(a, x0, y0) : 0, p1 = in1 ? bilin(b, x1, y1) : 0;
                    float tm = bilin(a, (float)x, (float)y) * (1 - t) + bilin(b, (float)x, (float)y) * t;
                    float val;
                    if (!in0 && !in1) {
                        val = tm;
                    } else {
                        // 双向一致性：从 a 走到 b 得到 v，再用 b 侧的反向场走回来，长度应互相抵消。
                        // 对不上说明这一点在中间帧被遮挡或矢量已经错了，此时错位平均就是重影/果冻，
                        // 按误差大小逐步退回时间插值，宁可略糊也不要果冻。
                        float ok = 1.0f;
                        // 只有「两侧 warp 后还对不上」才算真遮挡；对得上就保持锐利，别乱退化成糊
                        if ((opt & 2048) && gw > 0 && in0 && in1 && fabsf(p0 - p1) > 20.0f) {
                            Vec2 g = sample_vec_b(gfield, gw, gh, B, x + v.x, y + v.y);
                            float ex = v.x + g.x, ey = v.y + g.y;
                            float r = sqrtf(ex * ex + ey * ey);
                            ok = 1.0f - (r - 0.75f) / 2.25f;
                            if (ok < 0) ok = 0;
                            if (ok > 1) ok = 1;
                        }
                        float w0 = in0 ? (1 - t) : 0;
                        float w1 = in1 ? t : 0;
                        float warped = (w0 + w1 < 1e-4f) ? tm : (p0 * w0 + p1 * w1) / (w0 + w1);
                        val = warped * ok + tm * (1 - ok);
                    }
                    oy[(size_t)y * W + x] = (uint8_t)std::max(0, std::min(255, (int)lroundf(val)));
                }
            }
            // 色度：按 8x8 块矢量直接搬
            for (int y = 0; y < H / 2; y++) {
                for (int x = 0; x < W / 2; x++) {
                    Vec2 v = (opt & 256) ? sample_vec_b(cur, bw, bh, B, x * 2, y * 2) : sample_vech(cur, bw, bh, B, x * 2, y * 2);
                    float x0 = x - v.x * t * 0.5f, y0 = y - v.y * t * 0.5f;
                    float x1 = x + v.x * (1 - t) * 0.5f, y1 = y + v.y * (1 - t) * 0.5f;
                    const uint8_t *au = fa.data() + ysz, *av = au + csz;
                    const uint8_t *bu = fb.data() + ysz, *bv = bu + csz;
                    int hw = W / 2, hh = H / 2;
                    auto smp = [&](const uint8_t *pl, float sx, float sy) -> float {
                        sx = std::max(0.0f, std::min((float)hw - 1.001f, sx));
                        sy = std::max(0.0f, std::min((float)hh - 1.001f, sy));
                        int ix = (int)floorf(sx), iy = (int)floorf(sy);
                        int jx = std::min(hw - 1, ix + 1), jy = std::min(hh - 1, iy + 1);
                        float fx = sx - ix, fy = sy - iy;
                        float p = pl[(size_t)iy * hw + ix] * (1 - fx) + pl[(size_t)iy * hw + jx] * fx;
                        float q = pl[(size_t)jy * hw + ix] * (1 - fx) + pl[(size_t)jy * hw + jx] * fx;
                        return p * (1 - fy) + q * fy;
                    };
                    ou[(size_t)y * hw + x] = (uint8_t)std::max(0, std::min(255, (int)lroundf(smp(au, x0, y0) * (1 - t) + smp(bu, x1, y1) * t)));
                    ov[(size_t)y * hw + x] = (uint8_t)std::max(0, std::min(255, (int)lroundf(smp(av, x0, y0) * (1 - t) + smp(bv, x1, y1) * t)));
                }
            }
            fwrite(out.data(), 1, fsz, fo);
            done++;
        }
        fa.swap(fb);
    }
    // 收尾：最后一帧没有后继帧可插，直接照抄，否则每段少一帧、段尾会顿一下
    if (done > 0) {
        fwrite(fa.data(), 1, fsz, fo);
        done++;
    }
    fclose(fi); fclose(fo);
    fprintf(stderr, "wrote %d frames dup=%d\n", done, ndup);
    return 0;
}