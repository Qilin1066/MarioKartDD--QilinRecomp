// Pictures between two frames (interp.hpp): the pairing and the maths.
//
// Matrices are three rows of four (GX's Mtx), a point's rotation and scale in
// the left 3x3 and its translation in the last column.
#include "interp.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace mkdd::interp {

namespace {

// ---- small matrix maths

void multiply(const float a[12], const float b[12], float out[12]) {  // a after b
    float r[12];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) r[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j];
        r[4 * i + 3] = a[4 * i] * b[3] + a[4 * i + 1] * b[7] + a[4 * i + 2] * b[11] + a[4 * i + 3];
    }
    std::memcpy(out, r, sizeof r);
}
// The inverse of a rotation and a translation (a camera's view matrix).
void invert_rigid(const float m[12], float out[12]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[4 * i + j] = m[4 * j + i];
        out[4 * i + 3] = -(m[i] * m[3] + m[4 + i] * m[7] + m[8 + i] * m[11]);
    }
}
// The inverse of a matrix's 3x3; false if it has none.
bool invert3(const float m[12], float out[9]) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::fabs(det) > 1e-20f)) return false;
    const float k = 1.0f / det;
    out[0] = (e * i - f * h) * k, out[1] = (c * h - b * i) * k, out[2] = (b * f - c * e) * k;
    out[3] = (f * g - d * i) * k, out[4] = (a * i - c * g) * k, out[5] = (c * d - a * f) * k;
    out[6] = (d * h - e * g) * k, out[7] = (b * g - a * h) * k, out[8] = (a * e - b * d) * k;
    return true;
}

void quaternion_of(const float r[9], float q[4]) {  // r: a rotation, by rows; q: x, y, z, w
    const float trace = r[0] + r[4] + r[8];
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2;
        q[3] = 0.25f * s, q[0] = (r[7] - r[5]) / s, q[1] = (r[2] - r[6]) / s, q[2] = (r[3] - r[1]) / s;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        const float s = std::sqrt(1.0f + r[0] - r[4] - r[8]) * 2;
        q[3] = (r[7] - r[5]) / s, q[0] = 0.25f * s, q[1] = (r[1] + r[3]) / s, q[2] = (r[2] + r[6]) / s;
    } else if (r[4] > r[8]) {
        const float s = std::sqrt(1.0f + r[4] - r[0] - r[8]) * 2;
        q[3] = (r[2] - r[6]) / s, q[0] = (r[1] + r[3]) / s, q[1] = 0.25f * s, q[2] = (r[5] + r[7]) / s;
    } else {
        const float s = std::sqrt(1.0f + r[8] - r[0] - r[4]) * 2;
        q[3] = (r[3] - r[1]) / s, q[0] = (r[2] + r[6]) / s, q[1] = (r[5] + r[7]) / s, q[2] = 0.25f * s;
    }
}
void rotation_of(const float q[4], float r[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0] = 1 - 2 * (y * y + z * z), r[1] = 2 * (x * y - z * w), r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w), r[4] = 1 - 2 * (x * x + z * z), r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w), r[7] = 2 * (y * z + x * w), r[8] = 1 - 2 * (x * x + y * y);
}
// From a to b by t, the short way round.
void slerp(const float a[4], const float b[4], float t, float out[4]) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float sign = dot < 0 ? -1.0f : 1.0f;
    dot *= sign;
    float wa = 1 - t, wb = t;
    if (dot < 0.9995f) {
        const float angle = std::acos(dot), s = std::sin(angle);
        wa = std::sin((1 - t) * angle) / s, wb = std::sin(t * angle) / s;
    }
    float n = 0;
    for (int i = 0; i < 4; ++i) out[i] = wa * a[i] + wb * sign * b[i], n += out[i] * out[i];
    n = 1.0f / std::sqrt(n);
    for (int i = 0; i < 4; ++i) out[i] *= n;
}

}  // namespace

// ---- poses

namespace {

struct PoseParts {
    float q[4], s[3], t[3];
    bool rigid;
};
// A matrix as rotation, scale along its own axes, and translation; not rigid
// when its axes aren't at right angles (a shear) or one has no length.
PoseParts parts_of(const float m[12]) {
    PoseParts p{};
    float axis[3][3];
    p.rigid = true;
    for (int c = 0; c < 3; ++c) {
        const float x = m[c], y = m[4 + c], z = m[8 + c];
        p.s[c] = std::sqrt(x * x + y * y + z * z);
        if (!(p.s[c] > 1e-12f)) {
            p.rigid = false;
            return p;
        }
        axis[c][0] = x / p.s[c], axis[c][1] = y / p.s[c], axis[c][2] = z / p.s[c];
    }
    const auto dot = [&](int a, int b) { return axis[a][0] * axis[b][0] + axis[a][1] * axis[b][1] + axis[a][2] * axis[b][2]; };
    if (std::fabs(dot(0, 1)) > 2e-3f || std::fabs(dot(0, 2)) > 2e-3f || std::fabs(dot(1, 2)) > 2e-3f) {
        p.rigid = false;
        return p;
    }
    // A mirrored matrix: the mirror goes into the third axis's scale.
    const float det = axis[0][0] * (axis[1][1] * axis[2][2] - axis[1][2] * axis[2][1]) -
                      axis[0][1] * (axis[1][0] * axis[2][2] - axis[1][2] * axis[2][0]) +
                      axis[0][2] * (axis[1][0] * axis[2][1] - axis[1][1] * axis[2][0]);
    if (det < 0) {
        p.s[2] = -p.s[2];
        for (float& v : axis[2]) v = -v;
    }
    const float r[9] = {axis[0][0], axis[1][0], axis[2][0], axis[0][1], axis[1][1], axis[2][1], axis[0][2], axis[1][2], axis[2][2]};
    quaternion_of(r, p.q);
    p.t[0] = m[3], p.t[1] = m[7], p.t[2] = m[11];
    return p;
}
void matrix_of(const float q[4], const float s[3], const float t[3], float m[12]) {
    float r[9];
    rotation_of(q, r);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m[4 * i + j] = r[3 * i + j] * s[j];
        m[4 * i + 3] = t[i];
    }
}

}  // namespace

void blend_matrix(const float a[12], const float b[12], float t, float out[12]) {
    const PoseParts pa = parts_of(a), pb = parts_of(b);
    if (!pa.rigid || !pb.rigid) {
        for (int i = 0; i < 12; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
        return;
    }
    float q[4], s[3], tr[3];
    slerp(pa.q, pb.q, t, q);
    for (int i = 0; i < 3; ++i) s[i] = pa.s[i] + (pb.s[i] - pa.s[i]) * t, tr[i] = pa.t[i] + (pb.t[i] - pa.t[i]) * t;
    matrix_of(q, s, tr, out);
}

// ---- pairing

bool Blender::pair(const gx::Frame& prev, const gx::Frame& cur) {
    counts_ = {};
    const size_t loads = cur.loads.size(), views = cur.views.size();
    prepared_.resize(loads);
    for (Prepared& p : prepared_) p.how = 0;
    views_.resize(views);
    for (ViewPair& v : views_) v.blend = false;
    counts_.loads = uint32_t(loads), counts_.views = uint32_t(views);
    if (cur.state_info.size() != cur.states.size() || views == 0) return false;
    // Views pair by their order in the frame, which holds while there are as
    // many: a camera a player, drawn in the same order every frame. Another
    // count is a change of scene.
    if (prev.views.size() != views) {
        last_step_.assign(views, -1.0f);
        counts_.cuts = uint32_t(views);
        return false;
    }
    if (last_step_.size() != views) last_step_.assign(views, -1.0f);
    const auto from_parts = [](const PoseParts& p, Pose& o) {
        std::memcpy(o.q, p.q, sizeof o.q), std::memcpy(o.s, p.s, sizeof o.s), std::memcpy(o.t, p.t, sizeof o.t);
        o.rigid = p.rigid;
    };
    bool any = false;
    for (size_t v = 0; v < views; ++v) {
        ViewPair& vp = views_[v];
        const gx::View& a = prev.views[v], &b = cur.views[v];
        invert_rigid(b.m, vp.inverse);
        vp.has_projection = a.has_projection && b.has_projection;
        std::memcpy(vp.projection[0], a.projection, 24), std::memcpy(vp.projection[1], b.projection, 24);
        const PoseParts pa = parts_of(a.m), pb = parts_of(b.m);
        from_parts(pa, vp.from), from_parts(pb, vp.to);
        float eye_a[12];
        invert_rigid(a.m, eye_a);
        for (int i = 0; i < 3; ++i) vp.from.t[i] = eye_a[4 * i + 3], vp.to.t[i] = vp.inverse[4 * i + 3];  // the eyes, not the translations
        // A cut: a turn of more than 60 degrees in a tick, or the eye moving far beyond its last
        // step and beyond a hundredth of the view's depth. Then this view isn't blended.
        const float far_z = b.has_projection && b.projection[4] != 0 ? std::fabs(b.projection[5] / b.projection[4]) : 0.0f;
        float step = 0;
        for (int i = 0; i < 3; ++i) step += (vp.to.t[i] - vp.from.t[i]) * (vp.to.t[i] - vp.from.t[i]);
        step = std::sqrt(step);
        const float dot = std::fabs(pa.q[0] * pb.q[0] + pa.q[1] * pb.q[1] + pa.q[2] * pb.q[2] + pa.q[3] * pb.q[3]);
        const bool rigid = pa.rigid && pb.rigid && std::fabs(pa.s[0] - 1) < 1e-2f && std::fabs(pb.s[0] - 1) < 1e-2f && pa.s[2] > 0 && pb.s[2] > 0;
        const bool cut = !rigid || dot < 0.8660254f || (step > 0.01f * far_z && last_step_[v] >= 0 && step > 6.0f * last_step_[v]) ||
                         (last_step_[v] < 0 && far_z > 0 && step > 0.01f * far_z);
        last_step_[v] = cut ? -1.0f : step;
        vp.blend = !cut;
        if (cut) ++counts_.cuts;
        any |= !cut;
    }
    if (!any) return false;
    // Loads pair by tag and slot: the n-th of a kind in each frame, where both have as many.
    const auto keys_of = [](const gx::Frame& f, std::vector<Key>& keys) {
        keys.clear();
        for (size_t i = 0; i < f.loads.size(); ++i)
            if (f.loads[i].tag != 0 && f.loads[i].view != gx::kNoView)
                keys.push_back({(uint64_t(f.loads[i].tag) << 8) | f.loads[i].slot, uint32_t(i)});
        std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.key != b.key ? a.key < b.key : a.index < b.index; });
    };
    keys_of(prev, prev_keys_), keys_of(cur, cur_keys_);
    size_t p = 0;
    for (size_t c = 0; c < cur_keys_.size();) {
        const uint64_t key = cur_keys_[c].key;
        size_t c_end = c;
        while (c_end < cur_keys_.size() && cur_keys_[c_end].key == key) ++c_end;
        while (p < prev_keys_.size() && prev_keys_[p].key < key) ++p;
        size_t p_end = p;
        while (p_end < prev_keys_.size() && prev_keys_[p_end].key == key) ++p_end;
        const bool same = p_end - p == c_end - c;
        for (size_t i = c; i < c_end; ++i) {
            const gx::Load& l1 = cur.loads[cur_keys_[i].index];
            const ViewPair& vp = views_[l1.view];
            if (!vp.blend) continue;
            Prepared& pr = prepared_[cur_keys_[i].index];
            float m1[12];
            multiply(vp.inverse, l1.mv, m1);  // the model's own matrix: the view taken off
            std::memcpy(pr.to.m, m1, sizeof m1);
            pr.how = 2;
            const gx::Load* l0 = same ? &prev.loads[prev_keys_[p + (i - c)].index] : nullptr;
            if (l0 && l0->view == l1.view) {
                float inverse0[12], m0[12];
                invert_rigid(prev.views[l0->view].m, inverse0);
                multiply(inverse0, l0->mv, m0);
                // A jump (a rescue, a cannon, an item changing hands): more than a fiftieth of the
                // view's depth in a tick. It keeps its new place.
                const float far_z = vp.has_projection && vp.projection[1][4] != 0 ? std::fabs(vp.projection[1][5] / vp.projection[1][4]) : 0.0f;
                float moved = 0;
                for (int k = 0; k < 3; ++k) moved += (m1[4 * k + 3] - m0[4 * k + 3]) * (m1[4 * k + 3] - m0[4 * k + 3]);
                if (far_z > 0 && std::sqrt(moved) > 0.02f * far_z) {
                    ++counts_.jumped;
                } else {
                    from_parts(parts_of(m0), pr.from), from_parts(parts_of(m1), pr.to);
                    std::memcpy(pr.from.m, m0, sizeof m0);
                    std::memcpy(pr.mv0, l0->mv, sizeof pr.mv0);
                    pr.how = 1;
                }
            }
            if (pr.how == 1) ++counts_.paired;
            else ++counts_.alone;
        }
        c = c_end;
    }
    return true;
}

// ---- a picture's states

void Blender::states(const gx::Frame& cur, float phase, gx::State* out) {
    const size_t n = cur.states.size();
    if (n) std::memcpy(out, cur.states.data(), n * sizeof(gx::State));
    if (!(phase < 1.0f) || prepared_.size() != cur.loads.size() || views_.size() != cur.views.size()) return;
    const float t = std::max(phase, 0.0f);
    const size_t loads = cur.loads.size(), views = views_.size();
    blended_.resize(12 * (loads + 2 * views));
    float* const view_at = blended_.data() + 12 * loads;  // each view blended, then that times the newer view's inverse
    for (size_t v = 0; v < views; ++v) {
        const ViewPair& vp = views_[v];
        if (!vp.blend) continue;
        float q[4], eye[3], r[9];
        slerp(vp.from.q, vp.to.q, t, q);
        for (int i = 0; i < 3; ++i) eye[i] = vp.from.t[i] + (vp.to.t[i] - vp.from.t[i]) * t;
        rotation_of(q, r);
        float* m = view_at + 24 * v;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) m[4 * i + j] = r[3 * i + j];
            m[4 * i + 3] = -(r[3 * i] * eye[0] + r[3 * i + 1] * eye[1] + r[3 * i + 2] * eye[2]);
        }
        multiply(m, vp.inverse, m + 12);
    }
    for (size_t i = 0; i < loads; ++i) {
        const Prepared& pr = prepared_[i];
        if (pr.how == 0) continue;
        if (pr.how == 1 && t <= 0.0f) {
            std::memcpy(blended_.data() + 12 * i, pr.mv0, 48);
            continue;
        }
        const float* view = view_at + 24 * cur.loads[i].view;
        float model[12];
        if (pr.how == 2) {
            std::memcpy(model, pr.to.m, sizeof model);
        } else if (!pr.from.rigid || !pr.to.rigid) {
            for (int k = 0; k < 12; ++k) model[k] = pr.from.m[k] + (pr.to.m[k] - pr.from.m[k]) * t;
        } else {
            float q[4], s[3], tr[3];
            slerp(pr.from.q, pr.to.q, t, q);
            for (int k = 0; k < 3; ++k) s[k] = pr.from.s[k] + (pr.to.s[k] - pr.from.s[k]) * t, tr[k] = pr.from.t[k] + (pr.to.t[k] - pr.from.t[k]) * t;
            matrix_of(q, s, tr, model);
        }
        multiply(view, model, blended_.data() + 12 * i);
    }
    for (size_t i = 0; i < n; ++i) {
        // (Read from the frame's own states: `out` is the GPU's memory, slow to read back.)
        const gx::State& was = cur.states[i];
        gx::State& s = out[i];
        if (was.projection[3][2] != -1.0f) continue;  // flat drawing (orthographic): as the newer frame has it
        const gx::StateInfo& info = cur.state_info[i];
        for (unsigned slot = 0; slot < gx::kMatrixSlots; ++slot) {
            const uint32_t l = info.load[slot];
            if (l == gx::kNoLoad || prepared_[l].how == 0) continue;
            const float* m = blended_.data() + 12 * size_t(l);
            const float (*rows)[4] = was.posmtx + 3 * slot;
            // The normal matrix follows: the inverse transpose of what was done to the position matrix.
            float inv[9];
            if (invert3(m, inv)) {
                float e[9];  // the old 3x3 times the new one's inverse
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b) e[3 * a + b] = rows[a][0] * inv[b] + rows[a][1] * inv[3 + b] + rows[a][2] * inv[6 + b];
                const float (*nr)[4] = was.nrmmtx + 3 * slot;
                float (*to)[4] = s.nrmmtx + 3 * slot;
                for (int a = 0; a < 3; ++a)
                    for (int b = 0; b < 3; ++b) to[a][b] = e[a] * nr[0][b] + e[3 + a] * nr[1][b] + e[6 + a] * nr[2][b];
            }
            std::memcpy(s.posmtx + 3 * slot, m, 48);
        }
        if (info.view == gx::kNoView || !views_[info.view].blend) continue;
        const ViewPair& vp = views_[info.view];
        // Lights are in view space: they move with the camera.
        const float* move = view_at + 24 * info.view + 12;
        for (size_t k = 0; k < 8; ++k) {
            auto& light = s.lights[k];
            const auto& old = was.lights[k];
            const float p[3] = {old.pos[0], old.pos[1], old.pos[2]}, d[3] = {old.dir[0], old.dir[1], old.dir[2]};
            for (int a = 0; a < 3; ++a) {
                light.pos[a] = move[4 * a] * p[0] + move[4 * a + 1] * p[1] + move[4 * a + 2] * p[2] + move[4 * a + 3];
                light.dir[a] = move[4 * a] * d[0] + move[4 * a + 1] * d[1] + move[4 * a + 2] * d[2];
            }
        }
        // The projection (a boost widens the field of view), where it is the view's own.
        if (vp.has_projection) {
            const float now[6] = {was.projection[0][0], was.projection[0][2], was.projection[1][1], was.projection[1][2], was.projection[2][2], was.projection[2][3]};
            if (std::memcmp(now, vp.projection[1], sizeof now) == 0 && std::memcmp(vp.projection[0], vp.projection[1], sizeof now) != 0) {
                float p[6];
                for (int k = 0; k < 6; ++k) p[k] = vp.projection[0][k] + (vp.projection[1][k] - vp.projection[0][k]) * t;
                s.projection[0][0] = p[0], s.projection[0][2] = p[1], s.projection[1][1] = p[2], s.projection[1][2] = p[3];
                s.projection[2][2] = p[4], s.projection[2][3] = p[5];
            }
        }
    }
}

// ---- pacing

namespace {
std::atomic<uint8_t> g_rate{uint8_t(FrameRate::original)};
std::atomic<double> g_display_hz{0.0};
constexpr double kTickNs = 1e9 * 1.001 / 60.0;  // the game's tick: NTSC's 59.94 a second
}  // namespace

bool supported() { return mtxtag::installed(); }
void set_frame_rate(FrameRate rate, double display_hz) {
    g_rate.store(uint8_t(rate), std::memory_order_relaxed);
    g_display_hz.store(display_hz, std::memory_order_relaxed);
}
bool between_frames() {
    const FrameRate r = FrameRate(g_rate.load(std::memory_order_relaxed));
    return supported() && (r == FrameRate::display || r == FrameRate::unlimited);
}
bool unlimited() { return supported() && FrameRate(g_rate.load(std::memory_order_relaxed)) == FrameRate::unlimited; }
double phase(uint64_t arrived_ns, uint64_t now_ns) {
    double p = now_ns > arrived_ns ? double(now_ns - arrived_ns) / kTickNs : 0.0;
    if (FrameRate(g_rate.load(std::memory_order_relaxed)) == FrameRate::display) {
        // A display at n times the game's rate (within half a percent): n even steps.
        const double per_tick = g_display_hz.load(std::memory_order_relaxed) * kTickNs / 1e9, n = std::round(per_tick);
        if (n >= 1 && std::fabs(per_tick - n) < 0.005 * n) p = std::ceil(p * n - 0.5) / n;
    }
    return std::clamp(p, 1.0 / 64.0, 1.0);
}

}  // namespace mkdd::interp
