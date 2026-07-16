#pragma once

#include <QString>

namespace ktm::xc2 {

class Xc2Settings final {
public:
    static QString installRoot();
    static void setInstallRoot(const QString &root);
    static void clearInstallRoot();
};

} // namespace ktm::xc2
