// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "rm_scan.h"

#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle* file_handle) : file_handle_(file_handle) {
    // 从第一张数据页的“上一个槽位”开始，调用 next() 定位到第一条记录
    rid_ = {.page_no = RM_FIRST_RECORD_PAGE, .slot_no = -1};
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    const RmFileHdr& file_hdr = file_handle_->file_hdr_;
    while (rid_.page_no >= RM_FIRST_RECORD_PAGE && rid_.page_no < file_hdr.num_pages) {
        RmPageGuard page = file_handle_->fetch_page_guard(rid_.page_no);
        if (page.valid()) {
            const int slot_no = Bitmap::next_bit(true, page->bitmap, file_hdr.num_records_per_page, rid_.slot_no);
            if (slot_no < file_hdr.num_records_per_page) {
                rid_.slot_no = slot_no;
                return;
            }
        }
        // 当前页没有更多记录，转到下一页从头查找
        rid_.page_no++;
        rid_.slot_no = -1;
    }
    rid_ = {.page_no = RM_NO_PAGE, .slot_no = -1};
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const { return rid_.page_no == RM_NO_PAGE; }

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const { return rid_; }
