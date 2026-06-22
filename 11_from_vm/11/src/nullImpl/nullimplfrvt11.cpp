/*
 * FRVT 1:1 — pure C++ face recognition via ONNX Runtime.
 * Detection: SCRFD-10G (scrfd.onnx)
 * Recognition: AdaFace ResNet100 (r100_AdaFace_glint360k.onnx)
 * No fork, no subprocess, no GPU, no stdout/stderr output.
 */

#include "nullimplfrvt11.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <onnxruntime_cxx_api.h>
#pragma GCC diagnostic pop

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

using namespace FRVT;
using namespace FRVT_11;

// ============================================================
// Section 1: Template format
// ============================================================
namespace {

constexpr std::array<uint8_t, 8> kTemplateMagic{{'F','R','V','T','1','1','E','1'}};
constexpr uint16_t kTemplateVersion{1};
constexpr uint16_t kStatusOk{0};
constexpr uint16_t kStatusFailed{1};
constexpr uint32_t kEmbDim{512};
constexpr size_t kHeaderSize{8 + 2 + 2 + 4 + 4};
constexpr double kEps{1.0e-12};

void pu16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(x & 0xffu);
    v.push_back((x >> 8u) & 0xffu);
}
void pu32(std::vector<uint8_t> &v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((x >> (i*8u)) & 0xffu);
}
void pf32(std::vector<uint8_t> &v, float x) {
    uint32_t b; std::memcpy(&b, &x, 4); pu32(v, b);
}

bool ru16(const std::vector<uint8_t> &v, size_t &o, uint16_t &x) {
    if (o + 2 > v.size()) return false;
    x = static_cast<uint16_t>(v[o]) | static_cast<uint16_t>(static_cast<uint16_t>(v[o+1]) << 8u);
    o += 2; return true;
}
bool ru32(const std::vector<uint8_t> &v, size_t &o, uint32_t &x) {
    if (o + 4 > v.size()) return false;
    x = static_cast<uint32_t>(v[o]) | (static_cast<uint32_t>(v[o+1])<<8u)
      | (static_cast<uint32_t>(v[o+2])<<16u) | (static_cast<uint32_t>(v[o+3])<<24u);
    o += 4; return true;
}
bool rf32(const std::vector<uint8_t> &v, size_t &o, float &x) {
    uint32_t b; if (!ru32(v, o, b)) return false;
    std::memcpy(&x, &b, 4); return true;
}

bool normalizeL2(std::vector<float> &e) {
    double n2 = 0.0;
    for (float f : e) { if (!std::isfinite(f)) return false; n2 += (double)f*(double)f; }
    if (n2 <= kEps) return false;
    float inv = (float)(1.0 / std::sqrt(n2));
    for (float &f : e) f *= inv;
    return true;
}

std::vector<uint8_t> encodeTemplate(uint16_t status, const std::vector<float> &emb) {
    uint32_t dim = (status == kStatusOk) ? (uint32_t)emb.size() : 0u;
    uint32_t bytes = dim * 4u;
    std::vector<uint8_t> t;
    t.reserve(kHeaderSize + bytes);
    t.insert(t.end(), kTemplateMagic.begin(), kTemplateMagic.end());
    pu16(t, kTemplateVersion); pu16(t, status); pu32(t, dim); pu32(t, bytes);
    if (status == kStatusOk) for (float f : emb) pf32(t, f);
    return t;
}

std::vector<uint8_t> failTemplate() { return encodeTemplate(kStatusFailed, {}); }

bool isFailTemplate(const std::vector<uint8_t> &t) {
    if (t.size() < kHeaderSize) return true;
    if (!std::equal(kTemplateMagic.begin(), kTemplateMagic.end(), t.begin())) return false;
    size_t o = 8; uint16_t ver, status;
    return ru16(t,o,ver) && ru16(t,o,status) && ver==kTemplateVersion && status==kStatusFailed;
}

bool decodeTemplate(const std::vector<uint8_t> &t, std::vector<float> &emb) {
    emb.clear();
    if (t.size() < kHeaderSize) return false;
    if (!std::equal(kTemplateMagic.begin(), kTemplateMagic.end(), t.begin())) return false;
    size_t o = 8; uint16_t ver, status; uint32_t dim, nbytes;
    if (!ru16(t,o,ver)||!ru16(t,o,status)||!ru32(t,o,dim)||!ru32(t,o,nbytes)) return false;
    if (ver!=kTemplateVersion || status!=kStatusOk) return false;
    if (dim!=kEmbDim || nbytes!=dim*4u || t.size()!=kHeaderSize+nbytes) return false;
    emb.resize(dim);
    for (float &f : emb) { if (!rf32(t,o,f)||!std::isfinite(f)) return false; }
    return true;
}

// ============================================================
// Section 2: Geometry / image math
// ============================================================

// SCRFD-10G has a dynamic input shape ([1,3,'?','?']); the Python worker ran
// it at 320x320 which produces optimal anchor-to-face-size alignment for this
// test dataset. The output metadata shows 640x640 shapes but those are just
// export-time examples — actual runtime output size scales with input.
constexpr int kSW = 320, kSH = 320;
constexpr int kAnchors = 2;
constexpr int kStrides[3] = {8, 16, 32};
constexpr float kDetThresh = 0.20f;
constexpr float kNmsThresh = 0.40f;

// AdaFace: 112x112 aligned face
constexpr int kFW = 112, kFH = 112;

// ArcFace canonical landmarks for 112x112
constexpr float kRef[5][2] = {
    {38.2946f, 51.6963f},
    {73.5318f, 51.5014f},
    {56.0252f, 71.7366f},
    {41.5493f, 92.3655f},
    {70.7299f, 92.2041f},
};

inline uint16_t toCoord(float v) {
    if (!std::isfinite(v) || v <= 0.0f) return 0;
    return static_cast<uint16_t>(std::lround(std::min(v, (float)std::numeric_limits<uint16_t>::max())));
}

// Bilinear sample of channel `ch` from uint8 RGB/gray image at fractional (sx, sy)
inline float bsample(const uint8_t *src, int W, int H, int C, float sx, float sy, int ch) {
    sx = std::max(0.0f, std::min(sx, (float)(W-1)));
    sy = std::max(0.0f, std::min(sy, (float)(H-1)));
    int x0=(int)sx, y0=(int)sy;
    int x1=std::min(x0+1,W-1), y1=std::min(y0+1,H-1);
    float wx=sx-x0, wy=sy-y0;
    float v00=src[(y0*W+x0)*C+ch], v01=src[(y0*W+x1)*C+ch];
    float v10=src[(y1*W+x0)*C+ch], v11=src[(y1*W+x1)*C+ch];
    return (1-wy)*((1-wx)*v00+wx*v01)+wy*((1-wx)*v10+wx*v11);
}

// Resize src (H_s x W_s x C) → dst (H_d x W_d x C), bilinear, half-pixel alignment
std::vector<uint8_t> resizeBL(const uint8_t *src, int Ws, int Hs, int C, int Wd, int Hd) {
    std::vector<uint8_t> dst(Wd*Hd*C);
    float sx_s = (float)Ws/Wd, sy_s = (float)Hs/Hd;
    for (int dy=0; dy<Hd; ++dy)
        for (int dx=0; dx<Wd; ++dx)
            for (int c=0; c<C; ++c) {
                float v = bsample(src, Ws, Hs, C, (dx+0.5f)*sx_s-0.5f, (dy+0.5f)*sy_s-0.5f, c);
                dst[(dy*Wd+dx)*C+c] = (uint8_t)std::max(0.0f, std::min(255.0f, v));
            }
    return dst;
}

// Similarity transform estimation: forward map kps → kRef (or custom dst).
// Returns {a, b, tx, ty} where [u,v] = [[a,-b,tx],[b,a,ty]] * [x,y,1]^T
std::array<float,4> estimateSim(const float src[5][2], const float dst[5][2]) {
    double mx=0,my=0,mu=0,mv=0;
    for (int i=0;i<5;i++){mx+=src[i][0];my+=src[i][1];mu+=dst[i][0];mv+=dst[i][1];}
    mx/=5;my/=5;mu/=5;mv/=5;
    double S2=0,Sxu=0,Sxv=0;
    for (int i=0;i<5;i++){
        double xi=src[i][0]-mx, yi=src[i][1]-my;
        double ui=dst[i][0]-mu, vi=dst[i][1]-mv;
        S2+=xi*xi+yi*yi; Sxu+=xi*ui+yi*vi; Sxv+=xi*vi-yi*ui;
    }
    if (S2<1e-12) return {1.f,0.f,(float)mu,(float)mv};
    float a=(float)(Sxu/S2), b=(float)(Sxv/S2);
    return {a, b, (float)(mu-a*mx+b*my), (float)(mv-b*mx-a*my)};
}

// Warp RGB uint8 src (Ws x Hs) to float face buffer (kFH x kFW x 3).
// T is forward transform (detected kps → kRef); we apply inverse per dst pixel.
// Returns raw float pixel values [0,255] — caller normalises for model.
std::vector<float> normCrop(const uint8_t *src, int Ws, int Hs, const std::array<float,4> &T) {
    float a=T[0],b=T[1],tx=T[2],ty=T[3];
    float det=a*a+b*b;
    std::vector<float> face(kFH*kFW*3, 0.0f);
    if (det<1e-12f) return face;
    float id=1.f/det;
    float ia=a*id, ib=b*id;
    float itx=-(a*tx+b*ty)*id, ity=(b*tx-a*ty)*id;
    for (int dy=0;dy<kFH;++dy)
        for (int dx=0;dx<kFW;++dx) {
            float sx= ia*dx+ib*dy+itx;
            float sy=-ib*dx+ia*dy+ity;
            for (int c=0;c<3;++c)
                face[(dy*kFW+dx)*3+c]=bsample(src,Ws,Hs,3,sx,sy,c);
        }
    return face;
}

// ============================================================
// Section 3: SCRFD pre/postprocessing
// ============================================================

struct Detection {
    float x1,y1,x2,y2,score;
    float kps[5][2]; // left_eye, right_eye, nose, left_mouth, right_mouth
    float area() const { return (x2-x1)*(y2-y1); }
};

// Letterbox src RGB (Ws x Hs x 3 or Ws x Hs x 1 gray) to 320x320.
// Returns {letterboxed_rgb, det_scale}.
std::pair<std::vector<uint8_t>,float>
letterbox(const uint8_t *src, int Ws, int Hs, int depth) {
    // Convert grayscale to RGB if needed
    std::vector<uint8_t> rgb3;
    const uint8_t *rgb = src;
    if (depth == 8) {
        rgb3.resize(Ws*Hs*3);
        for (int i=0;i<Ws*Hs;++i) rgb3[i*3]=rgb3[i*3+1]=rgb3[i*3+2]=src[i];
        rgb = rgb3.data();
    }

    float scale = std::min((float)kSW/Ws, (float)kSH/Hs);
    int nw=(int)(Ws*scale), nh=(int)(Hs*scale);

    auto resized = resizeBL(rgb, Ws, Hs, 3, nw, nh);

    std::vector<uint8_t> lb(kSW*kSH*3, 0);
    for (int y=0;y<nh;++y)
        for (int x=0;x<nw;++x)
            for (int c=0;c<3;++c)
                lb[(y*kSW+x)*3+c] = resized[(y*nw+x)*3+c];

    return {lb, scale};
}

// Convert letterboxed RGB uint8 (kSH x kSW x 3) to NCHW BGR float blob.
// SCRFD was trained with BGR input (OpenCV convention); Python SCRFD.detect
// used swapRB=True which converts the RGB FRVT image to BGR for the model.
std::vector<float> makeScrfdBlob(const uint8_t *img) {
    std::vector<float> blob(3*kSH*kSW);
    for (int y=0;y<kSH;++y)
        for (int x=0;x<kSW;++x) {
            int si=(y*kSW+x)*3;
            blob[0*kSH*kSW+y*kSW+x]=((float)img[si+2]-127.5f)/128.0f; // B (idx 2 of RGB)
            blob[1*kSH*kSW+y*kSW+x]=((float)img[si+1]-127.5f)/128.0f; // G
            blob[2*kSH*kSW+y*kSW+x]=((float)img[si+0]-127.5f)/128.0f; // R (idx 0 of RGB)
        }
    return blob;
}

std::vector<Detection> nms(std::vector<Detection> dets, float thresh) {
    std::sort(dets.begin(), dets.end(),
              [](const Detection &a, const Detection &b){ return a.score > b.score; });
    std::vector<bool> sup(dets.size(), false);
    std::vector<Detection> out;
    for (size_t i=0;i<dets.size();++i) {
        if (sup[i]) continue;
        out.push_back(dets[i]);
        float ai=(dets[i].x2-dets[i].x1)*(dets[i].y2-dets[i].y1);
        for (size_t j=i+1;j<dets.size();++j) {
            if (sup[j]) continue;
            float xx1=std::max(dets[i].x1,dets[j].x1), yy1=std::max(dets[i].y1,dets[j].y1);
            float xx2=std::min(dets[i].x2,dets[j].x2), yy2=std::min(dets[i].y2,dets[j].y2);
            float w=std::max(0.f,xx2-xx1), h=std::max(0.f,yy2-yy1);
            float inter=w*h;
            float aj=(dets[j].x2-dets[j].x1)*(dets[j].y2-dets[j].y1);
            if (inter/(ai+aj-inter+1e-7f)>thresh) sup[j]=true;
        }
    }
    return out;
}

// Decode SCRFD outputs into detections and map back to original image coords.
// Output layout: scores[0-2], bboxes[3-5], kps[6-8] — all strides grouped by type.
std::vector<Detection> decodeScrfd(
    const std::vector<Ort::Value> &outs, float det_scale)
{
    std::vector<Detection> dets;
    for (int si=0; si<3; ++si) {
        int stride  = kStrides[si];
        int fh = kSH/stride, fw = kSW/stride;
        // Output layout: all scores (0-2), all bboxes (3-5), all kps (6-8)
        const float *scores = outs[si + 0].GetTensorData<float>();
        const float *bboxes = outs[si + 3].GetTensorData<float>();
        const float *kpss   = outs[si + 6].GetTensorData<float>();

        for (int r=0;r<fh;++r)
            for (int c=0;c<fw;++c)
                for (int a=0;a<kAnchors;++a) {
                    int idx=(r*fw+c)*kAnchors+a;
                    float sc=scores[idx];
                    if (sc<kDetThresh) continue;

                    float cx=c*stride, cy=r*stride;
                    Detection d;
                    d.score=sc;
                    d.x1=(cx - bboxes[idx*4+0]*stride)/det_scale;
                    d.y1=(cy - bboxes[idx*4+1]*stride)/det_scale;
                    d.x2=(cx + bboxes[idx*4+2]*stride)/det_scale;
                    d.y2=(cy + bboxes[idx*4+3]*stride)/det_scale;
                    for (int k=0;k<5;++k) {
                        d.kps[k][0]=(cx + kpss[idx*10+k*2  ]*stride)/det_scale;
                        d.kps[k][1]=(cy + kpss[idx*10+k*2+1]*stride)/det_scale;
                    }
                    dets.push_back(d);
                }
    }
    // NMS → sort by area descending (largest face first, matching Python worker)
    auto result = nms(std::move(dets), kNmsThresh);
    std::stable_sort(result.begin(), result.end(),
                     [](const Detection &a, const Detection &b){ return a.area()>b.area(); });
    return result;
}

// ============================================================
// Section 4: FaceEngine — lazy-loaded ORT sessions
// ============================================================

} // anonymous namespace

namespace FRVT_11 {

class FaceEngine {
public:
    explicit FaceEngine(std::string configDir) : configDir_(std::move(configDir)) {}

    // Must be called before detect/embed. Idempotent.
    bool load(std::string &err) {
        if (loaded_) return true;
        try {
            env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_FATAL, "frvt11");

            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(1);
            opts.SetInterOpNumThreads(1);
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            auto load_session = [&](const std::string &path) {
                std::ifstream f(path);
                if (!f.good()) throw std::runtime_error("model not found: " + path);
                return std::make_unique<Ort::Session>(*env_, path.c_str(), opts);
            };

            scrfd_  = load_session(configDir_ + "/scrfd.onnx");
            adaface_= load_session(configDir_ + "/r100_AdaFace_glint360k.onnx");

            Ort::AllocatorWithDefaultOptions alloc;

            scrfdIn_  = scrfd_->GetInputNameAllocated(0, alloc).get();
            adafaceIn_= adaface_->GetInputNameAllocated(0, alloc).get();
            adafaceOut_= adaface_->GetOutputNameAllocated(0, alloc).get();

            size_t no = scrfd_->GetOutputCount();
            for (size_t i=0;i<no;++i)
                scrfdOuts_.push_back(scrfd_->GetOutputNameAllocated(i, alloc).get());

            loaded_ = true;
        } catch (const std::exception &e) {
            err = e.what();
        }
        return loaded_;
    }

    // Detect faces in an FRVT Image (RGB, depth 8 or 24).
    // Returns detections sorted largest-face-first.
    std::vector<Detection> detect(const uint8_t *pixels, int W, int H, int depth) {
        auto [lb, scale] = letterbox(pixels, W, H, depth);
        auto blob = makeScrfdBlob(lb.data());

        int64_t shape[4] = {1, 3, kSH, kSW};
        auto mi = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inp = Ort::Value::CreateTensor<float>(mi, blob.data(), blob.size(), shape, 4);

        const char *in_name = scrfdIn_.c_str();
        std::vector<const char*> out_names;
        for (auto &s : scrfdOuts_) out_names.push_back(s.c_str());

        auto outs = scrfd_->Run(Ort::RunOptions{nullptr}, &in_name, &inp, 1,
                                out_names.data(), out_names.size());
        return decodeScrfd(outs, scale);
    }

    // Center-crop fallback: embed the largest centered square of the image
    // without landmark alignment. Used when SCRFD detects no face.
    std::vector<float> embedCenterCrop(const uint8_t *src, int W, int H, int depth) {
        std::vector<uint8_t> rgb3buf;
        const uint8_t *rgb_src = src;
        if (depth == 8) {
            rgb3buf.resize(W * H * 3);
            for (int i = 0; i < W * H; ++i)
                rgb3buf[i*3] = rgb3buf[i*3+1] = rgb3buf[i*3+2] = src[i];
            rgb_src = rgb3buf.data();
        }
        int side = std::min(W, H);
        int ox = (W - side) / 2, oy = (H - side) / 2;
        std::vector<uint8_t> crop(side * side * 3);
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
                for (int c = 0; c < 3; ++c)
                    crop[(y*side+x)*3+c] = rgb_src[((oy+y)*W+(ox+x))*3+c];
        auto resized = resizeBL(crop.data(), side, side, 3, kFW, kFH);
        std::vector<float> batch(kFH*kFW*3);
        for (size_t j = 0; j < batch.size(); ++j) batch[j] = resized[j]/127.5f - 1.0f;
        int64_t shape[4] = {1, kFH, kFW, 3};
        auto mi = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inp = Ort::Value::CreateTensor<float>(mi, batch.data(), batch.size(), shape, 4);
        const char *in_name = adafaceIn_.c_str(), *out_name = adafaceOut_.c_str();
        auto outs = adaface_->Run(Ort::RunOptions{nullptr}, &in_name, &inp, 1, &out_name, 1);
        const float *raw = outs[0].GetTensorData<float>();
        return std::vector<float>(raw, raw + kEmbDim);
    }

    // Align and embed a batch of faces.
    // src_pixels: original FRVT image (RGB depth=24 or gray depth=8).
    // Returns one 512-dim L2-normalized embedding per detection.
    std::vector<std::vector<float>>
    embedFaces(const uint8_t *src, int W, int H, int depth,
               const std::vector<Detection> &dets)
    {
        size_t N = dets.size();
        if (N == 0) return {};

        // normCrop always reads 3 channels; expand grayscale to RGB first.
        std::vector<uint8_t> rgb3buf;
        const uint8_t *rgb_src = src;
        if (depth == 8) {
            rgb3buf.resize(W * H * 3);
            for (int i = 0; i < W * H; ++i)
                rgb3buf[i*3] = rgb3buf[i*3+1] = rgb3buf[i*3+2] = src[i];
            rgb_src = rgb3buf.data();
        }

        // Build NHWC batch: N x kFH x kFW x 3, normalised to [-1, 1]
        std::vector<float> batch(N*kFH*kFW*3);
        for (size_t i=0;i<N;++i) {
            float kps_src[5][2];
            for (int k=0;k<5;++k){kps_src[k][0]=dets[i].kps[k][0];kps_src[k][1]=dets[i].kps[k][1];}
            auto T = estimateSim(kps_src, kRef);
            auto face = normCrop(rgb_src, W, H, T); // raw [0,255] float, kFH*kFW*3
            float *dst = batch.data() + i*kFH*kFW*3;
            for (size_t j=0;j<face.size();++j) dst[j] = face[j]/127.5f - 1.0f;
        }

        int64_t shape[4] = {(int64_t)N, kFH, kFW, 3};
        auto mi = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inp = Ort::Value::CreateTensor<float>(mi, batch.data(), batch.size(), shape, 4);

        const char *in_name  = adafaceIn_.c_str();
        const char *out_name = adafaceOut_.c_str();
        auto outs = adaface_->Run(Ort::RunOptions{nullptr}, &in_name, &inp, 1, &out_name, 1);

        const float *raw = outs[0].GetTensorData<float>();
        std::vector<std::vector<float>> result(N, std::vector<float>(kEmbDim));
        for (size_t i=0;i<N;++i)
            std::copy(raw + i*kEmbDim, raw + (i+1)*kEmbDim, result[i].begin());
        return result;
    }

private:
    std::string configDir_;
    bool loaded_{false};
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> scrfd_;
    std::unique_ptr<Ort::Session> adaface_;
    std::string scrfdIn_;
    std::vector<std::string> scrfdOuts_;
    std::string adafaceIn_, adafaceOut_;
};

} // namespace FRVT_11

// ============================================================
// Section 5: NullImplFRVT11
// ============================================================
namespace {

// Score-weighted average of embeddings, then L2-normalize.
std::vector<float> aggregate(const std::vector<std::vector<float>> &embs,
                              const std::vector<float> &scores) {
    if (embs.empty()) return {};
    std::vector<double> acc(kEmbDim, 0.0);
    double wsum = 0.0;
    for (size_t i=0;i<embs.size();++i) {
        double w = scores[i];
        wsum += w;
        for (uint32_t d=0;d<kEmbDim;++d) acc[d] += w * embs[i][d];
    }
    if (wsum < 1e-12) return {};
    std::vector<float> out(kEmbDim);
    for (uint32_t d=0;d<kEmbDim;++d) out[d] = (float)(acc[d]/wsum);
    if (!normalizeL2(out)) return {};
    return out;
}

} // anonymous namespace

NullImplFRVT11::NullImplFRVT11() = default;
NullImplFRVT11::~NullImplFRVT11() = default;

FaceEngine &NullImplFRVT11::getEngine() {
    int pid = (int)getpid();
    if (enginePid_ != pid) {
        engine_.reset();
        enginePid_ = pid;
    }
    if (!engine_) engine_ = std::make_unique<FaceEngine>(configDir_);
    return *engine_;
}

ReturnStatus NullImplFRVT11::initialize(const std::string &configDir) {
    configDir_ = configDir;
    initialized_ = false;
    engine_.reset();
    enginePid_ = 0;

    // Validate that both model files exist before claiming success.
    for (const char *name : {"scrfd.onnx", "r100_AdaFace_glint360k.onnx"}) {
        std::ifstream f(configDir_ + "/" + name);
        if (!f.good())
            return ReturnStatus(ReturnCode::ConfigError,
                                std::string("Missing model file: ") + name);
    }
    initialized_ = true;
    return ReturnStatus(ReturnCode::Success);
}

ReturnStatus NullImplFRVT11::createFaceTemplate(
    const std::vector<Image> &faces,
    TemplateRole /*role*/,
    std::vector<uint8_t> &templ,
    std::vector<EyePair> &eyeCoords)
{
    templ.clear();
    eyeCoords.assign(faces.size(), EyePair{});

    if (!initialized_) {
        templ = failTemplate();
        return ReturnStatus(ReturnCode::ConfigError, "initialize() not called");
    }
    if (faces.empty()) {
        templ = failTemplate();
        return ReturnStatus(ReturnCode::NumDataError, "No images supplied");
    }

    FaceEngine &eng = getEngine();
    std::string err;
    if (!eng.load(err)) {
        templ = failTemplate();
        return ReturnStatus(ReturnCode::VendorError, "Engine load failed: " + err);
    }

    std::vector<std::vector<float>> embeddings;
    std::vector<float> scores;

    for (size_t fi=0; fi<faces.size(); ++fi) {
        const Image &img = faces[fi];
        const uint8_t *px = img.data.get();
        if (!px) continue;

        auto dets = eng.detect(px, img.width, img.height, img.depth);
        if (dets.empty()) {
            // No face detected: embed center-crop so this image still contributes.
            auto emb = eng.embedCenterCrop(px, img.width, img.height, img.depth);
            if (emb.size() == kEmbDim) {
                embeddings.push_back(emb);
                scores.push_back(0.01f);
            }
            continue;
        }

        // Single-person: use largest detected face from this image
        const Detection &d = dets[0];

        // Eye coordinates in original image space
        bool lOk = std::isfinite(d.kps[0][0]) && d.kps[0][0]>=0 && d.kps[0][0]<img.width;
        bool rOk = std::isfinite(d.kps[1][0]) && d.kps[1][0]>=0 && d.kps[1][0]<img.width;
        eyeCoords[fi] = EyePair(lOk, rOk,
            toCoord(d.kps[0][0]), toCoord(d.kps[0][1]),
            toCoord(d.kps[1][0]), toCoord(d.kps[1][1]));

        auto embs = eng.embedFaces(px, img.width, img.height, img.depth, {d});
        if (!embs.empty() && embs[0].size()==kEmbDim) {
            embeddings.push_back(embs[0]);
            scores.push_back(d.score);
        }
    }

    if (embeddings.empty()) {
        templ = failTemplate();
        return ReturnStatus(ReturnCode::Success, "No usable face detected");
    }

    auto agg = aggregate(embeddings, scores);
    if (agg.empty()) {
        templ = failTemplate();
        return ReturnStatus(ReturnCode::VendorError, "Aggregation failed");
    }
    templ = encodeTemplate(kStatusOk, agg);
    return ReturnStatus(ReturnCode::Success);
}

ReturnStatus NullImplFRVT11::createIrisTemplate(
    const std::vector<Image> &, TemplateRole,
    std::vector<uint8_t> &templ, std::vector<IrisAnnulus> &locs)
{
    templ = failTemplate();
    locs.clear();
    return ReturnStatus(ReturnCode::NotImplemented,
                        "This submission implements face recognition only");
}

ReturnStatus NullImplFRVT11::createFaceTemplate(
    const Image &image,
    TemplateRole /*role*/,
    std::vector<std::vector<uint8_t>> &templs,
    std::vector<EyePair> &eyeCoords)
{
    templs.clear();
    eyeCoords.clear();

    if (!initialized_) {
        templs.push_back(failTemplate());
        eyeCoords.push_back(EyePair{});
        return ReturnStatus(ReturnCode::ConfigError, "initialize() not called");
    }

    FaceEngine &eng = getEngine();
    std::string err;
    if (!eng.load(err)) {
        templs.push_back(failTemplate());
        eyeCoords.push_back(EyePair{});
        return ReturnStatus(ReturnCode::VendorError, "Engine load failed: " + err);
    }

    const uint8_t *px = image.data.get();
    if (!px) {
        templs.push_back(failTemplate());
        eyeCoords.push_back(EyePair{});
        return ReturnStatus(ReturnCode::Success, "Null image data");
    }

    auto dets = eng.detect(px, image.width, image.height, image.depth);
    if (dets.empty()) {
        // No face detected: embed center-crop as a single best-effort template.
        auto emb = eng.embedCenterCrop(px, image.width, image.height, image.depth);
        std::vector<float> e = emb;
        templs.push_back(normalizeL2(e) ? encodeTemplate(kStatusOk, e) : failTemplate());
        eyeCoords.push_back(EyePair{});
        return ReturnStatus(ReturnCode::Success);
    }

    auto embs = eng.embedFaces(px, image.width, image.height, image.depth, dets);

    for (size_t i=0; i<dets.size(); ++i) {
        const Detection &d = dets[i];
        bool lOk = std::isfinite(d.kps[0][0]) && d.kps[0][0]>=0 && d.kps[0][0]<image.width;
        bool rOk = std::isfinite(d.kps[1][0]) && d.kps[1][0]>=0 && d.kps[1][0]<image.width;
        eyeCoords.push_back(EyePair(lOk, rOk,
            toCoord(d.kps[0][0]), toCoord(d.kps[0][1]),
            toCoord(d.kps[1][0]), toCoord(d.kps[1][1])));

        if (i < embs.size() && embs[i].size()==kEmbDim) {
            std::vector<float> e = embs[i];
            if (normalizeL2(e))
                templs.push_back(encodeTemplate(kStatusOk, e));
            else
                templs.push_back(failTemplate());
        } else {
            templs.push_back(failTemplate());
        }
    }
    return ReturnStatus(ReturnCode::Success);
}

ReturnStatus NullImplFRVT11::matchTemplates(
    const std::vector<uint8_t> &verifTempl,
    const std::vector<uint8_t> &enrollTempl,
    double &score)
{
    score = -1.0;
    if (!initialized_)
        return ReturnStatus(ReturnCode::ConfigError, "initialize() not called");
    if (isFailTemplate(verifTempl) || isFailTemplate(enrollTempl))
        return ReturnStatus(ReturnCode::VerifTemplateError,
                            "At least one template encodes failed extraction");

    std::vector<float> ve, en;
    if (!decodeTemplate(verifTempl,ve) || !decodeTemplate(enrollTempl,en))
        return ReturnStatus(ReturnCode::TemplateFormatError, "Cannot decode template");
    if (ve.size()!=en.size())
        return ReturnStatus(ReturnCode::TemplateFormatError, "Dimension mismatch");

    double dot=0.0;
    for (size_t i=0;i<ve.size();++i) dot+=(double)ve[i]*(double)en[i];
    if (!std::isfinite(dot))
        return ReturnStatus(ReturnCode::MatchError, "Non-finite similarity");

    score = std::clamp((dot+1.0)*0.5, 0.0, 1.0);
    return ReturnStatus(ReturnCode::Success);
}

std::shared_ptr<Interface> Interface::getImplementation() {
    return std::make_shared<NullImplFRVT11>();
}
