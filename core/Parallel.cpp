#include "Parallel.h"

#include <QJsonObject>
#include <thread>
#include <vector>

namespace odv {

Limiter& Limiter::instance() {
    static Limiter l;
    return l;
}

void Limiter::setLimit(const QString& key, int limit) {
    if (limit < 1) limit = 1;
    QMutexLocker lock(&mutex_);
    if (!sems_.contains(key)) {
        sems_.insert(key, std::make_shared<QSemaphore>(limit));
        limits_.insert(key, limit);
        return;
    }
    // Adjust an existing ceiling without disturbing in-flight permits.
    const int old = limits_.value(key, limit);
    if (limit > old) {
        sems_[key]->release(limit - old);
    } else if (limit < old) {
        // Reclaim slots lazily; acquire() blocks until they come back.
        sems_[key]->acquire(old - limit);
    }
    limits_[key] = limit;
}

int Limiter::limit(const QString& key) {
    QMutexLocker lock(&mutex_);
    return limits_.value(key, 1);
}

Limiter::Permit Limiter::acquire(const QString& key) {
    std::shared_ptr<QSemaphore> sem;
    {
        QMutexLocker lock(&mutex_);
        if (!sems_.contains(key)) {
            sems_.insert(key, std::make_shared<QSemaphore>(1));
            limits_.insert(key, 1);
        }
        sem = sems_.value(key);
    }
    sem->acquire();  // blocks outside the mutex, so other keys stay live
    return Permit(this, key);
}

void Limiter::release(const QString& key) {
    std::shared_ptr<QSemaphore> sem;
    {
        QMutexLocker lock(&mutex_);
        sem = sems_.value(key);
    }
    if (sem) sem->release();
}

Limiter::Permit::~Permit() {
    if (owner_) owner_->release(key_);
}

QVector<QJsonObject> parallelRun(int count,
                                 const std::function<QString(int)>& keyOf,
                                 const std::function<QJsonObject(int)>& fn) {
    QVector<QJsonObject> out(count);
    if (count <= 0) return out;
    if (count == 1) {
        auto permit = Limiter::instance().acquire(keyOf(0));
        out[0] = fn(0);
        return out;
    }

    // One thread per job. The limiter — not the thread count — is the throttle,
    // so a job whose backend is saturated simply parks here instead of piling
    // extra load onto someone else's inference server.
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([&, i] {
            auto permit = Limiter::instance().acquire(keyOf(i));
            out[i] = fn(i);
        });
    }
    for (auto& t : threads) t.join();
    return out;
}

}  // namespace odv
