/*
    SPDX-FileCopyrightText: 2026 OFD plugin contributors
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#ifndef OKULAR_GENERATOR_OFD_H
#define OKULAR_GENERATOR_OFD_H

#include "document.h"
#include <okular/core/generator.h>

class OFDGenerator : public Okular::Generator
{
    Q_OBJECT
    Q_INTERFACES(Okular::Generator)
public:
    OFDGenerator(QObject *parent, const QVariantList &args);
    bool loadDocument(const QString &fileName, QList<Okular::Page *> &pages) override;

protected:
    bool doCloseDocument() override;
    QImage image(Okular::PixmapRequest *request) override;

private:
    OFD::Document m_document;
};
#endif
