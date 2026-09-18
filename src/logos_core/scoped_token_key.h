#ifndef LOGOS_CORE_SCOPED_TOKEN_KEY_H
#define LOGOS_CORE_SCOPED_TOKEN_KEY_H

#include <QString>

namespace LogosCore {

// Keep the core's per-instance TokenManager identity independent of the
// protocol header version supplied by the embedding application.  The
// length-delimited form is injective even when either untrusted segment
// contains the separator used by older key formats.
inline QString scopedInstanceTokenKey(const QString& moduleName,
                                      const QString& instanceId)
{
    return QStringLiteral("logos.instance-token.v1/%1:%2/%3:%4")
        .arg(moduleName.size())
        .arg(moduleName)
        .arg(instanceId.size())
        .arg(instanceId);
}

} // namespace LogosCore

#endif // LOGOS_CORE_SCOPED_TOKEN_KEY_H
