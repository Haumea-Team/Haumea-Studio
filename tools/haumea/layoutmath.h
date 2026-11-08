// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace LayoutMath
{
using Point = std::array<float, 3>;
inline Point add(Point a, Point b)
{
	return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
inline Point mul(Point a, float s)
{
	return {a[0] * s, a[1] * s, a[2] * s};
}
inline Point sub(Point a, Point b)
{
	return add(a, mul(b, -1));
}
inline float dot(Point a, Point b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline Point cross(Point a, Point b)
{
	return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline float length(Point a)
{
	return std::sqrt(dot(a, a));
}
inline Point unit(Point a, Point fallback = {0, 1, 0})
{
	float d = length(a);
	return d > 1e-6f ? mul(a, 1 / d) : fallback;
}
inline Point mix(Point a, Point b, float t)
{
	return add(mul(a, 1 - t), mul(b, t));
}
// How a point's handles behave: computed from the neighbours (AUTO),
// opposite each other with their own lengths (ALIGNED), independent (FREE),
// or straight at the neighbours (VECTOR, a corner).
enum Handles { HANDLES_AUTO, HANDLES_ALIGNED, HANDLES_FREE, HANDLES_VECTOR, NUM_HANDLES };
struct Knot {
	Point p, in = {0, 0, 0}, out = {0, 0, 0};
	int handles = HANDLES_AUTO;
	float tilt = 0;	// degrees about the curve, eased to the next point's
};
inline Point bezier(const Knot &a, const Knot &b, float t)
{
	float s = 1 - t;
	return add(add(mul(a.p, s * s * s), mul(add(a.p, a.out), 3 * s * s * t)),
	           add(mul(add(b.p, b.in), 3 * s * t * t), mul(b.p, t * t * t)));
}
inline Point tangent(const Knot &a, const Knot &b, float t)
{
	float s = 1 - t;
	return add(add(mul(a.out, 3 * s * s), mul(sub(add(b.p, b.in), add(a.p, a.out)), 6 * s * t)),
	           mul(b.in, -3 * t * t));
}
inline void automaticHandles(std::vector<Knot> &v, bool closed)
{
	if (v.size() < 2)
		return;
	for (size_t i = 0; i < v.size(); i++)
		if (v[i].handles == HANDLES_VECTOR) {
			// A third of the way to each neighbour: straight segments meet here.
			bool first = !closed && i == 0, last = !closed && i + 1 == v.size();
			v[i].in = first ? Point{0, 0, 0} : mul(sub(v[(i + v.size() - 1) % v.size()].p, v[i].p), 1.f / 3);
			v[i].out = last ? Point{0, 0, 0} : mul(sub(v[(i + 1) % v.size()].p, v[i].p), 1.f / 3);
		} else if (v[i].handles == HANDLES_AUTO) {
			Point before = v[(i + v.size() - 1) % v.size()].p, after = v[(i + 1) % v.size()].p;
			if (!closed && i == 0)
				before = v[i].p;
			if (!closed && i + 1 == v.size())
				after = v[i].p;
			Point d = mul(sub(after, before), (!closed && (i == 0 || i + 1 == v.size())) ? 1.f / 3 : 1.f / 6);
			v[i].out = d;
			v[i].in = mul(d, -1);
		}
}
// de Casteljau subdivision preserves the curve exactly before a new point moves.
inline Knot split(Knot &a, Knot &b, float t = .5f)
{
	Point a1 = mix(a.p, add(a.p, a.out), t), b1 = mix(add(a.p, a.out), add(b.p, b.in), t),
	      c1 = mix(add(b.p, b.in), b.p, t);
	Point a2 = mix(a1, b1, t), b2 = mix(b1, c1, t), p = mix(a2, b2, t);
	a.out = sub(a1, a.p);
	b.in = sub(c1, b.p);
	// The split keeps the curve only if the handles stay as they are now.
	if (a.handles != HANDLES_FREE)
		a.handles = HANDLES_ALIGNED;
	if (b.handles != HANDLES_FREE)
		b.handles = HANDLES_ALIGNED;
	Knot k;
	k.p = p;
	k.in = sub(a2, p);
	k.out = sub(b2, p);
	k.handles = HANDLES_ALIGNED;
	k.tilt = a.tilt + (b.tilt - a.tilt) * t;
	return k;
}
struct Sample {
	Point p, tangent, normal;
	float distance = 0;
	float tilt = 0;	// degrees, from the points around it
};
// The curve as closely spaced points with their distance along it; empty if
// the knots aren't usable.
inline std::vector<Sample> densePoints(const std::vector<Knot> &knots, bool curved, bool closed, float spacing)
{
	std::vector<Sample> dense;
	if (knots.size() < 2 || !std::isfinite(spacing) || spacing < .01f)
		return dense;
	for (const Knot &k : knots) {
		if (!std::isfinite(k.tilt) || std::abs(k.tilt) > 3600)
			return dense;
		for (Point p : {k.p, k.in, k.out})
			for (float v : p)
				if (!std::isfinite(v) || std::abs(v) > 1000000)
					return dense;
	}
	float total = 0;
	size_t segments = closed ? knots.size() : knots.size() - 1;
	for (size_t i = 0; i < segments; i++) {
		const Knot &a = knots[i], &b = knots[(i + 1) % knots.size()];
		float extent = length(a.out) + length(sub(add(b.p, b.in), add(a.p, a.out))) + length(b.in);
		int steps =
		    curved ? std::max(16, std::min(256, (int)std::ceil(extent / std::max(.05f, spacing * .1f)))) : 1;
		for (int j = (i ? 1 : 0); j <= steps; j++) {
			float t = (float)j / steps;
			Sample s;
			s.p = curved ? bezier(a, b, t) : mix(a.p, b.p, t);
			s.tangent = unit(curved ? tangent(a, b, t) : sub(b.p, a.p));
			// Smoothstep, so a banked stretch eases in and out at its points.
			s.tilt = a.tilt + (b.tilt - a.tilt) * t * t * (3 - 2 * t);
			if (!dense.empty())
				total += length(sub(s.p, dense.back().p));
			s.distance = total;
			dense.push_back(s);
		}
	}
	if (total < .001f)
		dense.clear();
	return dense;
}
inline float curveLength(const std::vector<Knot> &knots, bool curved, bool closed)
{
	std::vector<Sample> dense = densePoints(knots, curved, closed, .1f);
	return dense.empty() ? 0 : dense.back().distance;
}
// Points every spacing metres from from to to, fractions of the length.
inline std::vector<Sample> sample(const std::vector<Knot> &knots, bool curved, bool closed, float spacing,
                                  size_t limit = 2000, float from = 0, float to = 1)
{
	std::vector<Sample> result;
	if (limit == 0 || !std::isfinite(from) || !std::isfinite(to))
		return result;
	from = std::max(0.f, std::min(1.f, from));
	to = std::max(from, std::min(1.f, to));
	std::vector<Sample> dense = densePoints(knots, curved, closed, spacing);
	if (dense.empty())
		return result;
	float total = dense.back().distance;
	float start = from * total, end = to * total;
	// A whole closed curve would put its last object on its first.
	bool wrap = closed && from <= 0 && to >= 1;
	size_t cursor = 1;
	// Multiplied, not summed, so an even count lands its last object on the end.
	float slack = std::max(.0001f, spacing * .001f);
	for (size_t i = 0;; i++) {
		float d = start + i * spacing;
		if (d > (wrap ? end - slack : end + slack) || result.size() >= limit)
			break;
		d = std::min(d, end);
		while (cursor + 1 < dense.size() && dense[cursor].distance < d)
			cursor++;
		const Sample &a = dense[cursor - 1], &b = dense[cursor];
		float f = b.distance > a.distance
		              ? std::max(0.f, std::min(1.f, (d - a.distance) / (b.distance - a.distance)))
		              : 0;
		Sample s;
		s.p = mix(a.p, b.p, f);
		s.tangent = curved ? unit(mix(a.tangent, b.tangent, f), a.tangent) : b.tangent;
		s.tilt = a.tilt + (b.tilt - a.tilt) * f;
		s.distance = d;
		Point n = {0, 0, 1};
		if (!result.empty()) {
			const Sample &prev = result.back();
			n = prev.normal;
			Point axis = cross(prev.tangent, s.tangent);
			float sine = length(axis), cosine = std::max(-1.f, std::min(1.f, dot(prev.tangent, s.tangent)));
			if (sine > 1e-5f) {
				axis = mul(axis, 1 / sine);
				n = add(add(mul(n, cosine), mul(cross(axis, n), sine)),
				        mul(axis, dot(axis, n) * (1 - cosine)));
			}
		}
		n = sub(n, mul(s.tangent, dot(n, s.tangent)));
		if (length(n) < 1e-4f) {
			n = {0, 1, 0};
			n = sub(n, mul(s.tangent, dot(n, s.tangent)));
		}
		s.normal = unit(n, {1, 0, 0});
		result.push_back(s);
	}
	return result;
}
} // namespace LayoutMath
