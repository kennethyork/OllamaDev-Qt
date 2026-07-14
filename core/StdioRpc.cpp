#include "StdioRpc.h"

#include <QtGlobal>
#include <cstdio>

#ifdef Q_OS_WIN
#include <io.h>
#define ODV_DUP _dup
#define ODV_DUP2 _dup2
#define ODV_WRITE _write
#define ODV_CLOSE _close
#define ODV_FD_OUT 1
#define ODV_FD_ERR 2
#else
#include <unistd.h>
#define ODV_DUP ::dup
#define ODV_DUP2 ::dup2
#define ODV_WRITE ::write
#define ODV_CLOSE ::close
#define ODV_FD_OUT STDOUT_FILENO
#define ODV_FD_ERR STDERR_FILENO
#endif

namespace odv {

StdioChannel::StdioChannel() {
    saved_ = ODV_DUP(ODV_FD_OUT);
    if (saved_ < 0) return;
    // Deliberately NOT fflush(stdout) first: anything already sitting in the stdio
    // buffer would be flushed onto the real fd 1 — corrupting the very channel we
    // are protecting. Leaving it buffered means it goes to stderr at the flush in
    // the destructor instead.
    ODV_DUP2(ODV_FD_ERR, ODV_FD_OUT);
}

StdioChannel::~StdioChannel() {
    if (saved_ < 0) return;
    fflush(stdout);  // drains to fd 1, which is still stderr
    ODV_DUP2(saved_, ODV_FD_OUT);
    ODV_CLOSE(saved_);
}

void StdioChannel::send(const QByteArray& payload) {
    if (saved_ < 0) return;
    QByteArray buf = payload;
    buf.append('\n');
    qint64 off = 0;
    while (off < buf.size()) {
        const auto n =
            ODV_WRITE(saved_, buf.constData() + off, static_cast<unsigned>(buf.size() - off));
        if (n <= 0) return;  // the client hung up; the read loop will see EOF too
        off += n;
    }
}

}  // namespace odv
