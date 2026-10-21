#pragma once

// Small rigid-transform helpers (quaternions x, y, z, w).

#include <array>
#include <cmath>

#include "raybotic/scan/tools.hpp"

namespace raybotic::scan::detail {

using Vec3 = std::array<double, 3>;
using Quat = std::array<double, 4>;

inline Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 scale(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalized(const Vec3& a) { return scale(a, 1.0 / norm(a)); }

inline double norm(const Quat& q) {
    return std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
}
inline Quat normalized(const Quat& q) {
    const double n = norm(q);
    return {q[0] / n, q[1] / n, q[2] / n, q[3] / n};
}
inline Quat multiply(const Quat& a, const Quat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
inline Quat conjugate(const Quat& q) { return {-q[0], -q[1], -q[2], q[3]}; }
inline Vec3 rotate(const Quat& q, const Vec3& v) {
    const Vec3 u{q[0], q[1], q[2]};
    const Vec3 t = scale(cross(u, v), 2.0);
    return add(add(v, scale(t, q[3])), cross(u, t));
}

inline Quat slerp(const Quat& a, Quat b, double alpha) {
    double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0) {
        for (auto& c : b) c = -c;
        d = -d;
    }
    if (d > 0.9995) {
        Quat q;
        for (int i = 0; i < 4; ++i) q[i] = a[i] + alpha * (b[i] - a[i]);
        return normalized(q);
    }
    const double theta = std::acos(d);
    const double s = std::sin(theta);
    const double wa = std::sin((1.0 - alpha) * theta) / s;
    const double wb = std::sin(alpha * theta) / s;
    Quat q;
    for (int i = 0; i < 4; ++i) q[i] = wa * a[i] + wb * b[i];
    return normalized(q);
}

/// a * b: b expressed in a's parent.
inline Pose compose(const Pose& a, const Pose& b) {
    Pose p;
    p.rotation = normalized(multiply(a.rotation, b.rotation));
    p.translation = add(rotate(a.rotation, b.translation), a.translation);
    return p;
}

inline Pose inverse(const Pose& a) {
    Pose p;
    p.rotation = conjugate(a.rotation);
    p.translation = scale(rotate(p.rotation, a.translation), -1.0);
    return p;
}

}  // namespace raybotic::scan::detail
