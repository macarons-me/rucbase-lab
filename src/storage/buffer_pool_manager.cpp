// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "buffer_pool_manager.h"

#include <cstring>
#include <mutex>

// Lab1 作业接口需要 DiskManager 的完整定义（write_page / read_page / allocate_page）；
// 头文件中只有前向声明，因此必须在 .cpp 中补上本包含。
#include "storage/disk_manager.h"

/*
 * 锁与不变量约定（改动 Victim 选择、页表或 pin_count 时务必同步）：
 *   I1 page_table_ 的键集合 == 缓冲池中 page_no 有效的帧集合 == 被占用的帧集合。
 *      free_list_ 中的帧、以及 delete_page() 归还的帧都不在 page_table_ 中。
 *   I2 Page::pin_count_ > 0 的帧一定不在 replacer_ 中；pin_count_ 归零时必须立即 unpin。
 *   I3 同一帧不会同时出现在 free_list_ 和 replacer_ 中：帧要么空闲，要么已被淘汰器接管。
 *   I4 只有 pin_count_ == 0 的帧才允许被淘汰或删除，因此调用方持有的 Page* 在 pin 期间稳定。
 * 所有会改动上述结构的操作都在 latch_ 保护下进行。磁盘 I/O 也必须在 latch_ 内完成：
 * DiskManager 用 "lseek + read/write" 两个系统调用定位页面，若并发线程在两次调用之间
 * 插入自己的 lseek，就会读错页或写错位置，因此定址与读写必须成对原子。
 */

/**
 * @description: 从free_list或replacer中得到可淘汰帧页的 *frame_id
 * @return {bool} true: 可替换帧查找成功 , false: 可替换帧查找失败
 * @param {frame_id_t*} frame_id 帧页id指针,返回成功找到的可替换帧id
 */
bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    // 1 使用BufferPoolManager::free_list_判断缓冲池是否已满需要淘汰页面
    // 1.1 未满获得frame
    if (!free_list_.empty()) {
        // 还有从未装载过页面的帧，优先使用，省去一次写回
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }

    // 1.2 已满使用lru_replacer中的方法选择淘汰页面
    // 淘汰器只登记 pin_count_ 为 0 的帧，因此这里取出的帧必然无人持有
    return replacer_->victim(frame_id);
}

/**
 * @description: 更新页面数据, 如果为脏页则需写入磁盘，再更新为新页面，更新page元数据(data, is_dirty, page_id)和page
 * table
 * @param {Page*} page 写回页指针
 * @param {PageId} new_page_id 新的page_id
 * @param {frame_id_t} new_frame_id 新的帧frame_id
 */
void BufferPoolManager::update_page(Page* page, PageId new_page_id, frame_id_t new_frame_id) {
    // 1 如果是脏页，写回磁盘，并且把dirty置为false
    // 未被占用过的空闲帧满足 page->id_.page_no == INVALID_PAGE_ID，此时没有任何数据需要写回
    if (page->is_dirty_ && page->id_.page_no != INVALID_PAGE_ID) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }

    // 2 更新page table：摘掉旧页的映射（空闲帧没有旧页），登记新页
    if (page->id_.page_no != INVALID_PAGE_ID) {
        page_table_.erase(page->id_);
    }
    page_table_[new_page_id] = new_frame_id;

    // 3 重置page的data，更新page id
    // 帧可能属于上一个被淘汰的页，必须清空旧内容，避免读到上一个页面的残留数据
    page->reset_memory();
    page->id_ = new_page_id;
}

/**
 * @description: 从buffer pool获取需要的页。
 *              如果页表中存在page_id（说明该page在缓冲池中），并且pin_count++。
 *              如果页表不存在page_id（说明该page在磁盘中），则找缓冲池victim
 * page，将其替换为磁盘中读取的page，pin_count置1。
 * @return {Page*} 若获得了需要的页则将其返回，否则返回nullptr
 * @param {PageId} page_id 需要获取的页的PageId
 */
Page* BufferPoolManager::fetch_page(PageId page_id) {
    std::scoped_lock lock{latch_};

    //  1.     从page_table_中搜寻目标页
    //  1.1    若目标页有被page_table_记录，则将其所在frame固定(pin)，并返回目标页。
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        const frame_id_t frame_id = it->second;
        Page* page = &pages_[frame_id];
        // 页面已在内存中，直接固定并返回。pin_count_ 由 0 变为 1 时需从淘汰器摘除，
        // 否则该帧可能被重复选中淘汰，破坏不变量 I2。
        if (page->pin_count_ == 0) {
            replacer_->pin(frame_id);
        }
        page->pin_count_++;
        return page;
    }

    //  1.2    否则，尝试调用find_victim_page获得一个可用的frame，若失败则返回nullptr
    frame_id_t frame_id = INVALID_FRAME_ID;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }

    //  2.     若获得的可用frame存储的为dirty page，则须调用updata_page将page写回到磁盘
    //  3.     更新页表和页面
    Page* page = &pages_[frame_id];
    update_page(page, page_id, frame_id);

    //  4.     调用disk_manager_的read_page读取目标页到frame，并固定该页
    //
    // 读盘必须在 latch_ 之内完成：DiskManager 用 "lseek(fd, offset) + read(fd)" 两个
    // 系统调用定位页面，二者之间若被其它线程插入自己的 lseek，本次 read 就会读到别的页。
    // 缓冲池是这些系统调用的调用者，只有把 I/O 一起放进临界区，才能保证同一 fd 上的
    // 定址与读写成对原子。若把读盘放到锁外，ConcurrencyTest 会读到错页数据并触发
    // DiskManager::read_page Error。
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->pin_count_ = 1;
    replacer_->pin(frame_id);

    //  5.     返回目标页
    return page;
}

/**
 * @description: 取消固定pin_count>0的在缓冲池中的page
 * @return {bool} 如果目标页的pin_count<=0则返回false，否则返回true
 * @param {PageId} page_id 目标page的page_id
 * @param {bool} is_dirty 若目标page应该被标记为dirty则为true，否则为false
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    // 0. lock latch
    std::scoped_lock lock{latch_};

    // 1. 尝试在page_table_中搜寻page_id对应的页P
    // 1.1 P在页表中不存在 return false
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    const frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];

    // 1.2 P在页表中存在，获取其pin_count_
    // 2.1 若pin_count_已经等于0，则返回false
    if (page->pin_count_ <= 0) {
        return false;
    }

    // 2.2 若pin_count_大于0，则pin_count_自减一
    page->pin_count_--;

    // 2.2.1 若自减后等于0，则调用replacer_的unpin
    // 页面可能被多个线程同时使用，只有最后一个持有者释放后才允许淘汰
    if (page->pin_count_ == 0) {
        replacer_->unpin(frame_id);
    }

    // 3 根据参数is_dirty，更改P的is_dirty_
    if (is_dirty) {
        page->is_dirty_ = true;
    }

    return true;
}

/**
 * @description: 将目标页写回磁盘，不考虑当前页面是否正在被使用
 * @return {bool} 成功则返回true，否则返回false(只有page_table_中没有目标页时)
 * @param {PageId} page_id 目标页的page_id，不能为INVALID_PAGE_ID
 */
bool BufferPoolManager::flush_page(PageId page_id) {
    // 0. lock latch
    std::scoped_lock lock{latch_};

    // 1. 查找页表,尝试获取目标页P
    // 1.1 目标页P没有被page_table_记录 ，返回false
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    Page* page = &pages_[it->second];

    // 2. 无论P是否为脏都将其写回磁盘。
    // 页面可能正被其他线程固定使用，flush 与它的读写并不互斥；持有 latch_ 可以保证
    // 帧在此期间不会被淘汰或复用。
    disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);

    // 3. 更新P的is_dirty_
    page->is_dirty_ = false;

    return true;
}

/**
 * @description: 创建一个新的page，即从磁盘中移动一个新建的空page到缓冲池某个位置。
 * @return {Page*} 返回新创建的page，若创建失败则返回nullptr
 * @param {PageId*} page_id 当成功创建一个新的page时存储其page_id
 */
Page* BufferPoolManager::new_page(PageId* page_id) {
    // 1.   获得一个可用的frame，若无法获得则返回nullptr
    frame_id_t frame_id = INVALID_FRAME_ID;
    Page* page = nullptr;
    {
        std::scoped_lock lock{latch_};
        if (!find_victim_page(&frame_id)) {
            return nullptr;
        }

        // 2.   在fd对应的文件分配一个新的page_id
        // 分配策略是页面编号自增，因此需要在占用帧之前完成，避免分配失败留下脏状态
        const page_id_t new_page_no = disk_manager_->allocate_page(page_id->fd);
        const PageId new_page_id{.fd = page_id->fd, .page_no = new_page_no};

        // 3.   将frame的数据写回磁盘，并更新页表和页面元数据
        // 旧页若是脏页，update_page 会先把它写回磁盘
        page = &pages_[frame_id];
        update_page(page, new_page_id, frame_id);

        // 4.   固定frame，更新pin_count_
        page->pin_count_ = 1;
        replacer_->pin(frame_id);

        // 传出新建页面的编号
        page_id->page_no = new_page_no;
    }

    // 5.   返回获得的page
    // 新页面内容是 update_page 清空后的全零数据，无需从磁盘读取
    return page;
}

/**
 * @description: 从buffer_pool删除目标页
 * @return {bool} 如果目标页不存在于buffer_pool或者成功被删除则返回true，若其存在于buffer_pool但无法删除则返回false
 * @param {PageId} page_id 目标页
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    // 1.   在page_table_中查找目标页，若不存在返回true
    std::scoped_lock lock{latch_};

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return true;
    }
    const frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];

    // 2.   若目标页的pin_count不为0，则返回false
    // 只有引用次数为0的页面才能被删除，否则正在使用该页面的线程会读到被复用的帧
    if (page->pin_count_ != 0) {
        return false;
    }

    // 3.   将目标页数据写回磁盘，从页表中删除目标页，重置其元数据，将其加入free_list_，返回true
    // 脏页先落盘再回收，避免尚未写回的数据随帧一起丢失
    if (page->is_dirty_) {
        disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    }
    // 从淘汰器摘除：帧即将变为空闲，不应再作为淘汰对象（不变量 I3）
    replacer_->pin(frame_id);
    page_table_.erase(it);
    page->reset_memory();
    page->id_ = PageId{};
    page->is_dirty_ = false;
    page->pin_count_ = 0;
    free_list_.push_back(frame_id);

    return true;
}

/**
 * @description: 将buffer_pool中的所有页写回到磁盘
 * @param {int} fd 文件句柄
 */
void BufferPoolManager::flush_all_pages(int fd) {
    std::scoped_lock lock{latch_};

    for (const auto& [page_id, frame_id] : page_table_) {
        if (page_id.fd != fd) {
            continue;
        }
        // 该文件的页面都要落盘，无论脏标志如何，刷新后脏标志清零
        Page* page = &pages_[frame_id];
        disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }
}
