// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "rm_file_handle.h"

#include <cstring>

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    RmPageGuard page = fetch_page_guard(rid.page_no);
    if (!page.valid()) {
        throw PageNotExistError(std::to_string(fd_), rid.page_no);
    }
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page->bitmap, rid.slot_no)) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    auto record = std::make_unique<RmRecord>(file_hdr_.record_size);
    memcpy(record->data, page->get_slot(rid.slot_no), file_hdr_.record_size);
    return record;
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {const char*} buf 要插入的记录数据，只读取 record_size 字节
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(const char* buf, Context* context) {
    RmPageGuard page = create_free_page_guard();
    if (!page.valid()) {
        throw InternalError("failed to obtain a free record page");
    }

    const int slot_no = Bitmap::first_bit(false, page->bitmap, file_hdr_.num_records_per_page);
    if (slot_no >= file_hdr_.num_records_per_page) {
        throw InternalError("free record page has no empty slot");
    }

    memcpy(page->get_slot(slot_no), buf, file_hdr_.record_size);
    Bitmap::set(page->bitmap, slot_no);
    page->page_hdr->num_records++;
    page.mark_dirty();

    const int page_no = page->page->get_page_id().page_no;
    // 页面已满，从空闲页链表头摘除，后续插入会另取空闲页或新建页
    if (page->page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page->page_hdr->next_free_page_no;
    }
    return Rid{.page_no = page_no, .slot_no = slot_no};
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {const char*} buf 要插入的记录数据，只读取 record_size 字节
 */
void RmFileHandle::insert_record(const Rid& rid, const char* buf) {
    // 后续恢复/回滚实验使用的扩展接口，不属于 Lab1 评分任务。
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    RmPageGuard page = fetch_page_guard(rid.page_no);
    if (!page.valid()) {
        throw PageNotExistError(std::to_string(fd_), rid.page_no);
    }
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page->bitmap, rid.slot_no)) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    const bool was_full = page->page_hdr->num_records == file_hdr_.num_records_per_page;
    Bitmap::reset(page->bitmap, rid.slot_no);
    page->page_hdr->num_records--;
    page.mark_dirty();

    // 满页删除一条记录后变为未满，需要重新挂回空闲页链表
    if (was_full) {
        push_free_page(page);
    }
}

/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {const char*} buf 新记录的数据，只读取 record_size 字节
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, const char* buf, Context* context) {
    RmPageGuard page = fetch_page_guard(rid.page_no);
    if (!page.valid()) {
        throw PageNotExistError(std::to_string(fd_), rid.page_no);
    }
    if (rid.slot_no < 0 || rid.slot_no >= file_hdr_.num_records_per_page ||
        !Bitmap::is_set(page->bitmap, rid.slot_no)) {
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    memcpy(page->get_slot(rid.slot_no), buf, file_hdr_.record_size);
    page.mark_dirty();
}

/**
 * 以下为实现细节使用的辅助函数，单元测试不直接调用。
 */

/**
 * @description: 分配一张新数据页，初始化页头与 bitmap，并挂到空闲页链表头
 * @return {RmPageGuard} 新页面的 guard（已固定、已置脏）
 */
RmPageGuard RmFileHandle::create_new_page_guard() {
    PageId new_page_id{.fd = fd_, .page_no = INVALID_PAGE_ID};
    PageGuard guard = buffer_pool_manager_->new_page_guard(&new_page_id);
    if (!guard) {
        return {};
    }
    RmPageGuard page(&file_hdr_, std::move(guard));

    file_hdr_.num_pages++;
    page->page_hdr->num_records = 0;
    // 新页作为空闲链表头，后继指向原链表头
    page->page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = new_page_id.page_no;
    Bitmap::init(page->bitmap, file_hdr_.bitmap_size);
    page.mark_dirty();
    return page;
}

/**
 * @description: 获取一张未满数据页的 guard：优先复用空闲链表头，否则新建一页
 * @return {RmPageGuard} 空闲页面的 guard（已固定）
 */
RmPageGuard RmFileHandle::create_free_page_guard() {
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        return create_new_page_guard();
    }
    return fetch_page_guard(file_hdr_.first_free_page_no);
}

/**
 * @description: 页面从已满变为未满时，把它重新挂到空闲页链表头
 */
void RmFileHandle::push_free_page(RmPageGuard& page) {
    page->page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page->page->get_page_id().page_no;
}
