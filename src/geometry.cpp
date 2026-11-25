/*
    SPDX-FileCopyrightText: 2026 OFD plugin contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "geometry.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace OFD
{
bool appendArc(QPainterPath &path, const QList<double> &v)
{
    if (v.size() != 7 || (v[3] != 0 && v[3] != 1) || (v[4] != 0 && v[4] != 1)) {
        return false;
    }
    const QPointF start = path.currentPosition();
    const QPointF end(v[5], v[6]);
    if (start == end) {
        return true;
    }
    double rx = std::abs(v[0]);
    double ry = std::abs(v[1]);
    if (rx < 1e-12 || ry < 1e-12) {
        path.lineTo(end);
        return true;
    }
    // Endpoint-to-centre conversion follows SVG's elliptical arc notes:
    // https://www.w3.org/TR/SVG/implnote.html#ArcConversionEndpointToCenter
    constexpr double pi = std::numbers::pi;
    const double phi = std::fmod(v[2], 360.0) * pi / 180;
    const double cosPhi = std::cos(phi);
    const double sinPhi = std::sin(phi);
    const QPointF half = (start - end) / 2;
    const double xp = cosPhi * half.x() + sinPhi * half.y();
    const double yp = -sinPhi * half.x() + cosPhi * half.y();
    const double correction = std::hypot(xp / rx, yp / ry);
    if (correction > 1) {
        rx *= correction;
        ry *= correction;
    }
    if (!std::isfinite(rx) || !std::isfinite(ry) || rx > 1e9 || ry > 1e9) {
        return false;
    }
    const double u = xp / rx;
    const double w = yp / ry;
    const double length = u * u + w * w;
    if (length == 0) {
        return false;
    }
    const double factor = (v[3] == v[4] ? -1 : 1) * (std::sqrt(std::max(0.0, 1 - length)) / std::sqrt(length));
    const double cxp = factor * rx * w;
    const double cyp = -factor * ry * u;
    const QPointF centre(cosPhi * cxp - sinPhi * cyp + (start.x() + end.x()) / 2, sinPhi * cxp + cosPhi * cyp + (start.y() + end.y()) / 2);
    const QPointF from((xp - cxp) / rx, (yp - cyp) / ry);
    const QPointF to((-xp - cxp) / rx, (-yp - cyp) / ry);
    double theta = std::atan2(from.y(), from.x());
    double delta = std::atan2(from.x() * to.y() - from.y() * to.x(), QPointF::dotProduct(from, to));
    if (v[4] == 0 && delta > 0) {
        delta -= 2 * pi;
    } else if (v[4] == 1 && delta < 0) {
        delta += 2 * pi;
    }
    if (!std::isfinite(theta) || !std::isfinite(delta) || !std::isfinite(centre.x()) || !std::isfinite(centre.y())) {
        return false;
    }
    const int segments = std::clamp(int(std::ceil(std::abs(delta) / (pi / 2))), 1, 4);
    const double step = delta / segments;
    const double k = 4.0 / 3 * std::tan(step / 4);
    const auto point = [&](double angle) {
        return centre + QPointF(cosPhi * rx * std::cos(angle) - sinPhi * ry * std::sin(angle), sinPhi * rx * std::cos(angle) + cosPhi * ry * std::sin(angle));
    };
    const auto tangent = [&](double angle) {
        return QPointF(-cosPhi * rx * std::sin(angle) - sinPhi * ry * std::cos(angle), -sinPhi * rx * std::sin(angle) + cosPhi * ry * std::cos(angle));
    };
    // Cubic segments span at most 90 degrees. Preserve the declared endpoint
    // exactly so adjoining path segments do not accumulate conversion error.
    for (int i = 0; i < segments; ++i) {
        path.cubicTo(point(theta) + k * tangent(theta), point(theta + step) - k * tangent(theta + step), i + 1 == segments ? end : point(theta + step));
        theta += step;
    }
    return true;
}
}
