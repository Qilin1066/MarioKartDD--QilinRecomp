// Pictures between two frames (interp.hpp): the pairing and the maths.
//
// Matrices are three rows of four (GX's Mtx), a point's rotation and scale in
// the left 3x3 and its translation in the last column.
#include "interp.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

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

// ---- MKDD_INTERP_LOG=<file>: what pairing finds, every 600 frames (ten seconds): how many
// loads pair and why the rest don't, how far views and models move in a tick as a share of the
// view's depth (the cut and jump rules are in those units), and the tags that pair worst.
namespace {
struct Probe {
    FILE* out = nullptr;
    uint64_t frames = 0, blendable = 0, loads = 0, paired = 0, jumped = 0, fresh = 0, miscounted = 0, other_view = 0, untagged = 0,
             views = 0, cuts = 0, view_changes = 0, textures = 0, texture_snaps = 0;
    uint32_t view_step[14] = {}, view_turn[8] = {}, model_move[14] = {};
    double far_sum = 0;
    float far_max = 0, step_max = 0;
    struct Tag {
        uint32_t paired = 0, miscounted = 0, fresh = 0, jumped = 0;
    };
    std::unordered_map<uint32_t, Tag> tags;
    static unsigned bucket(float ratio) {  // [2^(i-15), 2^(i-14)); 0 below, 13 above 1/4
        if (!(ratio > 0)) return 0;
        return unsigned(std::clamp(int(std::floor(std::log2(ratio))) + 15, 0, 13));
    }
    void turn(float dot) {
        const float degrees = 2.0f * std::acos(std::min(dot, 1.0f)) * 57.29578f;
        static constexpr float kEdges[7] = {0.5f, 1, 2, 5, 10, 20, 45};
        unsigned b = 0;
        while (b < 7 && degrees >= kEdges[b]) ++b;
        ++view_turn[b];
    }
    void flush() {
        std::fprintf(out, "frames %llu (blendable %llu, view-count changes %llu): a frame %.1f loads: paired %.1f, jumped %.2f, new %.1f, "
                          "count differs %.1f, view cut or other %.1f, untagged %.1f; views %.2f, cuts %llu\n",
                     (unsigned long long)frames, (unsigned long long)blendable, (unsigned long long)view_changes, double(loads) / double(frames),
                     double(paired) / double(frames), double(jumped) / double(frames), double(fresh) / double(frames),
                     double(miscounted) / double(frames), double(other_view) / double(frames), double(untagged) / double(frames),
                     double(views) / double(frames), (unsigned long long)cuts);
        std::fprintf(out, "  texture matrices a frame: blended %.1f, wrapped or jumped %.2f\n", double(textures) / double(frames), double(texture_snaps) / double(frames));
        const auto row = [&](const char* name, const uint32_t* h, unsigned n) {
            std::fprintf(out, "  %s:", name);
            for (unsigned i = 0; i < n; ++i) std::fprintf(out, " %u", h[i]);
            std::fprintf(out, "\n");
        };
        std::fprintf(out, "  far: mean %.0f, largest %.0f; largest view step %.1f\n", views ? far_sum / double(views) : 0.0, double(far_max), double(step_max));
        row("view step / far, by powers of two from 2^-15 (first: none; last: over 1/4)", view_step, 14);
        row("view turn, degrees <0.5 <1 <2 <5 <10 <20 <45 more", view_turn, 8);
        row("model move / far, the same buckets", model_move, 14);
        std::vector<std::pair<uint32_t, Tag>> worst(tags.begin(), tags.end());
        std::sort(worst.begin(), worst.end(), [](const auto& a, const auto& b) {
            return a.second.miscounted + a.second.fresh + a.second.jumped > b.second.miscounted + b.second.fresh + b.second.jumped;
        });
        for (size_t i = 0; i < worst.size() && i < 12; ++i) {
            const Tag& t = worst[i].second;
            if (t.miscounted + t.fresh + t.jumped == 0) break;
            std::fprintf(out, "  tag %08X (%s): paired %u, count differs %u, new %u, jumped %u\n", worst[i].first,
                         (worst[i].first & 1) ? "indexed" : (worst[i].first & 2) ? "caller" : "packet", t.paired, t.miscounted, t.fresh, t.jumped);
        }
        std::fprintf(out, "  %llu tags in all\n", (unsigned long long)tags.size());
        std::fflush(out);
        const auto keep = out;
        *this = Probe{};
        out = keep;
    }
};
Probe* const g_probe = [] () -> Probe* {
    const char* path = std::getenv("MKDD_INTERP_LOG");
    FILE* f = path && *path ? std::fopen(path, "w") : nullptr;
    if (!f) return nullptr;
    auto* p = new Probe;
    p->out = f;
    return p;
}();
}  // namespace

// ---- pairing

// The rules for what isn't blended, from the attract mode's demo races (MKDD_INTERP_LOG,
// 2026-11-26; docs/graphics.md has the distributions), as shares of the view's far distance:
// a camera's eye moves about 1/4000 of it a tick in a race and never 1/500, and turns under
// 10 degrees; at the demos' cuts it moves 1/64 to 1/16 or turns over 45 degrees. A model's
// own movement is mostly under 1/2000 and tails off by 1/250; what is beyond is a rescue, a
// cannon, or the n-th load of a tag not being the thing it was (two objects swapping places
// in the draw order), and both are better drawn where the newer frame has them.
constexpr float kCutTurn = 0.9848078f;   // the quaternions' dot at a turn of 20 degrees
constexpr float kCutStep = 1.0f / 256.0f;
constexpr float kJump = 1.0f / 128.0f;

bool Blender::pair(const gx::Frame& prev, const gx::Frame& cur) {
    counts_ = {};
    tex_.clear();
    const size_t loads = cur.loads.size(), views = cur.views.size();
    prepared_.resize(loads);
    for (Prepared& p : prepared_) p.how = 0;
    views_.resize(views);
    for (ViewPair& v : views_) v.blend = false;
    counts_.loads = uint32_t(loads), counts_.views = uint32_t(views);
    if (g_probe) {
        if (g_probe->frames >= 600) g_probe->flush();
        ++g_probe->frames, g_probe->loads += loads, g_probe->views += views;
        for (const gx::Load& l : cur.loads) g_probe->untagged += l.tag == 0 || l.view == gx::kNoView;
        if (prev.views.size() != views) ++g_probe->view_changes;
    }
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
        const bool cut = !rigid || dot < kCutTurn || (step > kCutStep * far_z && last_step_[v] >= 0 && step > 6.0f * last_step_[v]) ||
                         (last_step_[v] < 0 && far_z > 0 && step > kCutStep * far_z);
        if (g_probe) {
            g_probe->far_sum += far_z, g_probe->far_max = std::max(g_probe->far_max, far_z), g_probe->step_max = std::max(g_probe->step_max, step);
            ++g_probe->view_step[far_z > 0 ? Probe::bucket(step / far_z) : 0];
            g_probe->turn(dot);
            g_probe->cuts += cut;
        }
        last_step_[v] = cut ? -1.0f : step;
        vp.blend = !cut;
        if (cut) ++counts_.cuts;
        any |= !cut;
    }
    if (!any) return false;
    if (g_probe) ++g_probe->blendable;
    // Pair within each view: one player's visibility must not change another
    // player's counts or shift its n-th load to a different object.
    const auto keys_of = [](const gx::Frame& f, std::vector<Key>& keys) {
        keys.clear();
        for (size_t i = 0; i < f.loads.size(); ++i)
            if (f.loads[i].tag != 0 && f.loads[i].view != gx::kNoView)
                keys.push_back({(uint64_t(f.loads[i].tag) << 32) | f.loads[i].id, f.loads[i].slot, f.loads[i].view, uint32_t(i)});
        std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
            return a.key != b.key ? a.key < b.key : a.slot != b.slot ? a.slot < b.slot : a.view != b.view ? a.view < b.view : a.index < b.index;
        });
    };
    keys_of(prev, prev_keys_), keys_of(cur, cur_keys_);
    size_t p = 0;
    for (size_t c = 0; c < cur_keys_.size();) {
        const uint64_t key = cur_keys_[c].key;
        const uint32_t slot = cur_keys_[c].slot;
        const uint32_t view = cur_keys_[c].view;
        const auto before = [&](const Key& k) { return k.key != key ? k.key < key : k.slot != slot ? k.slot < slot : k.view < view; };
        const auto same_kind = [&](const Key& k) { return k.key == key && k.slot == slot && k.view == view; };
        size_t c_end = c;
        while (c_end < cur_keys_.size() && same_kind(cur_keys_[c_end])) ++c_end;
        while (p < prev_keys_.size() && before(prev_keys_[p])) ++p;
        size_t p_end = p;
        while (p_end < prev_keys_.size() && same_kind(prev_keys_[p_end])) ++p_end;
        const bool same = p_end - p == c_end - c;
        for (size_t i = c; i < c_end; ++i) {
            const gx::Load& l1 = cur.loads[cur_keys_[i].index];
            const ViewPair& vp = views_[l1.view];
            if (g_probe) {
                Probe::Tag& t = g_probe->tags[l1.tag];
                if (!vp.blend) ++g_probe->other_view;
                else if (p_end == p) ++g_probe->fresh, ++t.fresh;
                else if (!same) ++g_probe->miscounted, ++t.miscounted;
            }
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
                if (g_probe && far_z > 0) ++g_probe->model_move[Probe::bucket(std::sqrt(moved) / far_z)];
                if (far_z > 0 && std::sqrt(moved) > kJump * far_z) {
                    ++counts_.jumped;
                    if (g_probe) ++g_probe->jumped, ++g_probe->tags[l1.tag].jumped;
                } else {
                    from_parts(parts_of(m0), pr.from), from_parts(parts_of(m1), pr.to);
                    std::memcpy(pr.from.m, m0, sizeof m0);
                    std::memcpy(pr.mv0, l0->mv, sizeof pr.mv0);
                    pr.prev = prev_keys_[p + (i - c)].index;
                    pr.how = 1;
                }
            }
            if (g_probe && pr.how == 1) ++g_probe->paired, ++g_probe->tags[l1.tag].paired;
            else if (g_probe && l0 && l0->view != l1.view) ++g_probe->other_view;
            if (pr.how == 1) ++counts_.paired;
            else ++counts_.alone;
        }
        c = c_end;
    }
    pair_textures(prev, cur);
    if (g_probe) g_probe->textures += counts_.textures, g_probe->texture_snaps += counts_.texture_snaps;
    // Nothing to move (a movie, a menu: flat drawing only): the picture between would be the frame.
    return counts_.paired + counts_.alone > 0;
}

// A texture matrix whose translation moves by more than a quarter of the texture in a tick has
// wrapped (a scroll kept in 0..1) or jumped: it is drawn as the newer frame has it.
constexpr float kTextureJump = 0.25f;
constexpr size_t kTexRows = offsetof(gx::State, posmtx) + 30 * sizeof(float[4]), kTexBytes = 30 * sizeof(float[4]);

void Blender::pair_textures(const gx::Frame& prev, const gx::Frame& cur) {
    tex_.clear();
    if (!counts_.paired || prev.state_info.size() != prev.states.size()) return;
    prev_state_of_.assign(prev.loads.size(), gx::kNoLoad);
    for (size_t j = 0; j < prev.state_info.size(); ++j)
        for (uint32_t l : prev.state_info[j].load)
            if (l < prev_state_of_.size() && prev_state_of_[l] == gx::kNoLoad) prev_state_of_[l] = uint32_t(j);
    float to[30][4];
    for (size_t i = 0; i < cur.states.size(); ++i) {
        float w;  // projection[3][2]: -1 for a perspective projection (flat drawing isn't blended)
        cur.states.read(i, offsetof(gx::State, projection) + 14 * sizeof(float), sizeof w, &w);
        if (w != -1.0f) continue;
        uint32_t j = gx::kNoLoad;
        for (uint32_t l : cur.state_info[i].load)
            if (l != gx::kNoLoad && prepared_[l].how == 1 && prepared_[l].prev < prev_state_of_.size()) {
                j = prev_state_of_[prepared_[l].prev];
                break;
            }
        if (j == gx::kNoLoad) continue;
        TexBlend& tb = tex_.emplace_back();
        tb.state = uint32_t(i), tb.groups = 0;
        prev.states.read(j, kTexRows, kTexBytes, tb.from);
        cur.states.read(i, kTexRows, kTexBytes, to);
        for (unsigned g = 0; g < 10; ++g) {
            if (std::memcmp(tb.from[3 * g], to[3 * g], 3 * sizeof(float[4])) == 0) continue;
            bool jump = false;
            for (unsigned r = 3 * g; r < 3 * g + 3; ++r) jump |= !(std::fabs(to[r][3] - tb.from[r][3]) <= kTextureJump);
            if (jump) ++counts_.texture_snaps;
            else tb.groups |= 1u << g, ++counts_.textures;
        }
        if (!tb.groups) tex_.pop_back();
    }
}

// ---- a picture's states

void Blender::states(const gx::Frame& cur, float phase, gx::State* out) {
    const size_t n = cur.states.size();
    cur.states.resolve_all(out);
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
        thread_local gx::State was;  // (the frame keeps its states as blocks)
        cur.states.resolve(i, was);
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
    float to[30][4];
    for (const TexBlend& tb : tex_) {
        cur.states.read(tb.state, kTexRows, kTexBytes, to);
        float (*rows)[4] = out[tb.state].posmtx + 30;
        for (unsigned g = 0; g < 10; ++g)
            if ((tb.groups >> g) & 1)
                for (unsigned r = 3 * g; r < 3 * g + 3; ++r)
                    for (unsigned k = 0; k < 4; ++k) rows[r][k] = tb.from[r][k] + (to[r][k] - tb.from[r][k]) * t;
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


