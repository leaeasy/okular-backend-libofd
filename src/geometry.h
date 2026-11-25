/*
    SPDX-FileCopyrightText: 2026 OFD plugin contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef OKULAR_OFD_GEOMETRY_H
#define OKULAR_OFD_GEOMETRY_H

#include <QList>
#include <QPainterPath>

namespace OFD
{
// OFD A: rx ry rotation large-arc sweep end-x end-y.
bool appendArc(QPainterPath &path, const QList<double> &parameters);
}
#endif
