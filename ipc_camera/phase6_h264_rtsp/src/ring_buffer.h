#pragma once
/**
 * ring_buffer.h — 线程安全环形缓冲 (header-only)
 *
 * 用于编码线程 → RTSP 线程之间的 H.264 帧传递。
 * H.264 有 GOP 结构, P 帧依赖 I 帧, 需要缓存多帧供新客户端从 IDR 开始。
 */

#include <vector>
#include <atomic>
#include <pthread.h>

template<typename T>
class RingBuffer {
    std::vector<T>   buf_;
    size_t           head_ = 0, tail_ = 0, count_ = 0, max_;
    pthread_mutex_t  mutex_  = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t   cond_r_ = PTHREAD_COND_INITIALIZER;  // 可读 (消费者等待)
    pthread_cond_t   cond_w_ = PTHREAD_COND_INITIALIZER;  // 可写 (生产者等待)
    std::atomic<bool> running_{true};

public:
    explicit RingBuffer(size_t n) : buf_(n), max_(n) {}

    // 阻塞 push, 满时等待。返回 false 表示 shutdown
    bool push(T&& item) {
        pthread_mutex_lock(&mutex_);
        while (count_ >= max_ && running_) {
            pthread_cond_wait(&cond_w_, &mutex_);
        }
        if (!running_) { pthread_mutex_unlock(&mutex_); return false; }
        buf_[head_] = std::move(item);
        head_ = (head_ + 1) % max_;
        count_++;
        pthread_cond_signal(&cond_r_);
        pthread_mutex_unlock(&mutex_);
        return true;
    }

    // 阻塞 pop, 空时等待。返回 false 表示 shutdown
    bool pop(T& item) {
        pthread_mutex_lock(&mutex_);
        while (count_ == 0 && running_) {
            pthread_cond_wait(&cond_r_, &mutex_);
        }
        if (!running_) { pthread_mutex_unlock(&mutex_); return false; }
        item = std::move(buf_[tail_]);
        tail_ = (tail_ + 1) % max_;
        count_--;
        pthread_cond_signal(&cond_w_);
        pthread_mutex_unlock(&mutex_);
        return true;
    }

    // 非阻塞 pop, 空时返回 false (shutdown 也返回 false)
    bool try_pop(T& item) {
        pthread_mutex_lock(&mutex_);
        if (count_ == 0) {
            pthread_mutex_unlock(&mutex_);
            return false;
        }
        item = std::move(buf_[tail_]);
        tail_ = (tail_ + 1) % max_;
        count_--;
        pthread_cond_signal(&cond_w_);
        pthread_mutex_unlock(&mutex_);
        return true;
    }

    // 唤醒所有等待者 (shutdown)
    void wake_all() {
        running_ = false;
        pthread_cond_broadcast(&cond_r_);
        pthread_cond_broadcast(&cond_w_);
    }

    bool empty() const { return count_ == 0; }
    size_t size() const { return count_; }
};
