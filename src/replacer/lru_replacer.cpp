// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "lru_replacer.h"

/*
 * 头尾约定（实现前提，改动时务必同步 victim 与 unpin）：
 *   LRUlist_ 的首部 = 最近被访问（MRU），尾部 = 最久未被访问（LRU）。
 *   unpin() 将 frame 插入首部，victim() 从尾部取出淘汰对象。
 *   两者必须互相对应，一旦错配，淘汰次序会整体反转。
 *
 * 维护的不变量：
 *   I1 LRUhash_ 的键集合与 LRUlist_ 的元素集合互为镜像（同生同灭）。
 *   I2 LRUlist_.size() == LRUhash_.size() == Size()。
 *   I3 集合中只含 unpinned 的 frame。
 *   I4 同一 frame 在 LRUlist_ 中至多出现一次（由 unpin 查重保证）。
 *   I5 LRUlist_.size() 不超过 max_size_（与缓冲池容量一致）。
 * 所有公共函数均在 latch_ 保护下同时维护两个容器，因此上述不变量对外始终成立。
 */
LRUReplacer::LRUReplacer(size_t num_pages) { max_size_ = num_pages; }

LRUReplacer::~LRUReplacer() = default;

/**
 * @description: 使用LRU策略删除一个victim frame，并返回该frame的id
 * @param {frame_id_t*} frame_id 被移除的frame的id，如果没有frame被移除返回nullptr
 * @return {bool} 如果成功淘汰了一个页面则返回true，否则返回false
 */
bool LRUReplacer::victim(frame_id_t* frame_id) {
    // std::scoped_lock 同时保护容量和链表状态。
    // 它能够避免死锁发生，其构造函数能够自动进行上锁操作，析构函数会对互斥量进行解锁操作，保证线程安全。
    std::scoped_lock lock{latch_};  //  如果编译报错可以替换成其他lock

    if (LRUlist_.empty()) {
        // 没有可淘汰对象，不写入 *frame_id，交由调用方判断返回值
        return false;
    }

    // 尾部为最久未被访问的一端，即本次的淘汰对象
    const frame_id_t victim_frame = LRUlist_.back();
    LRUlist_.pop_back();
    LRUhash_.erase(victim_frame);  // 同步清理哈希表，维持不变量 I1

    *frame_id = victim_frame;
    return true;
}

/**
 * @description: 固定指定的frame，即该页面无法被淘汰
 * @param {frame_id_t} 需要固定的frame的id
 */
void LRUReplacer::pin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};

    auto it = LRUhash_.find(frame_id);
    if (it == LRUhash_.end()) {
        // 该 frame 不在可淘汰集合中（已被淘汰，或本就处于 pinned 状态），为空操作
        return;
    }

    // 借助哈希表中保存的迭代器，删除链表节点为 O(1)
    LRUlist_.erase(it->second);
    LRUhash_.erase(it);
}

/**
 * @description: 取消固定一个frame，代表该页面可以被淘汰
 * @param {frame_id_t} frame_id 取消固定的frame的id
 */
void LRUReplacer::unpin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};

    if (LRUhash_.find(frame_id) != LRUhash_.end()) {
        // 已在可淘汰集合中：重复 unpin 既不插入重复元素，也不改变其淘汰次序（不变量 I4）
        return;
    }

    LRUlist_.push_front(frame_id);  // 首部为最近被访问端
    LRUhash_[frame_id] = LRUlist_.begin();
}

/**
 * @description: 获取当前replacer中可以被淘汰的页面数量
 */
size_t LRUReplacer::Size() {
    std::scoped_lock lock{latch_};
    return LRUlist_.size();
}
