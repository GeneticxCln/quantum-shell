// Reading the shell's frozen abstract IPC socket out of the kernel's own table, for tests that must
// know whether a *specific* shell process is listening rather than trust that one is.
//
// The frozen abstract socket name is per user rather than per process, so "the shell is listening"
// is ambiguous whenever more than one shell could exist — a second Quantum Shell binds nothing and
// carries on, and then every assertion made through the socket is answered by whichever process got
// there first. The inode is what disambiguates: each bind is its own kernel object, and a test that
// records the inode at start-up can tell its own shell's socket from any other.
//
// Taken from tests/integration/niri_live_layershell_test.cpp.
#pragma once

#include "ipc/IPCProtocol.h"

#include <QFile>
#include <QString>
#include <QByteArray>
#include <QList>
#include <optional>

// The frozen abstract socket name as an address. The leading NUL is what makes it abstract rather than a
// filesystem entry, and /proc prints that NUL as '@' — one of them, exactly as `ss -x` does.
inline QString abstractSocketRow()
{
    return QStringLiteral("@%1").arg(QString::fromLatin1(quantum::ipc::SocketName));
}

// The socket inode the kernel has bound to that name, or nothing when no process holds it.
inline std::optional<qint64> boundSocketInode()
{
    QFile table(QStringLiteral("/proc/net/unix"));
    if (!table.open(QIODevice::ReadOnly))
        return std::nullopt;

    const QList<QByteArray> lines = table.readAll().split('\n');
    for (const QByteArray& line : lines) {
        const QList<QByteArray> fields = line.simplified().split(' ');
        // Eight columns: the counters, the flags, the type and state, the inode and the path (`Num RefCount
        // Protocol Flags Type St Inode Path`). A row with no path is a connected socket rather than a listening
        // one, which is why the last field is compared rather than trusted to exist.
        if (fields.size() < 8)
            continue;
        if (fields.constLast() == abstractSocketRow().toUtf8())
            return fields.at(6).toLongLong();
    }
    return std::nullopt;
}
