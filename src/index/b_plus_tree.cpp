// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "b_plus_tree.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <memory>

#include "common/errors.h"
#include "index_scan.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "storage/page.h"

int compare_index_key(const char* left, const char* right, ColType type, int column_length) {
    if (left == nullptr || right == nullptr || column_length <= 0) {
        throw InternalError("Invalid index comparison input");
    }
    switch (type) {
        case TYPE_INT: {
            if (column_length != static_cast<int>(sizeof(int))) {
                throw InternalError("Invalid integer index key length");
            }
            int left_value;
            int right_value;
            memcpy(&left_value, left, sizeof(left_value));
            memcpy(&right_value, right, sizeof(right_value));
            return (left_value < right_value) ? -1 : ((left_value > right_value) ? 1 : 0);
        }
        case TYPE_FLOAT: {
            if (column_length != static_cast<int>(sizeof(float))) {
                throw InternalError("Invalid float index key length");
            }
            float left_value;
            float right_value;
            memcpy(&left_value, left, sizeof(left_value));
            memcpy(&right_value, right, sizeof(right_value));
            return (left_value < right_value) ? -1 : ((left_value > right_value) ? 1 : 0);
        }
        case TYPE_STRING:
            return memcmp(left, right, column_length);
        default:
            throw InternalError("Unexpected data type");
    }
}

int compare_index_key(const char* left,
                      const char* right,
                      const std::vector<ColType>& column_types,
                      const std::vector<int>& column_lengths) {
    if (column_types.size() != column_lengths.size()) {
        throw InternalError("Invalid composite index metadata");
    }
    int offset = 0;
    for (size_t i = 0; i < column_types.size(); ++i) {
        int result = compare_index_key(left + offset, right + offset, column_types[i], column_lengths[i]);
        if (result != 0) return result;
        offset += column_lengths[i];
    }
    return 0;
}

BPlusTreeNode::BPlusTreeNode(const IndexFileHeader* file_header, Page* page) : file_header_(file_header), page_(page) {
    page_header_ = reinterpret_cast<IndexPageHeader*>(page_->get_data());
    keys_ = page_->get_data() + sizeof(IndexPageHeader);
    rids_ = reinterpret_cast<Rid*>(keys_ + file_header_->keys_region_size_);
}

page_id_t BPlusTreeNode::get_page_no() const noexcept { return page_->get_page_id().page_no; }

PageId BPlusTreeNode::get_page_id() const noexcept { return page_->get_page_id(); }

page_id_t BPlusTreeNode::remove_and_return_only_child() {
    assert(get_size() == 1);
    page_id_t child_page_no = value_at(0);
    erase_pair(0);
    assert(get_size() == 0);
    return child_page_no;
}

int BPlusTreeNode::find_child(const BPlusTreeNode* child) const {
    int child_index = 0;
    while (child_index < page_header_->key_count && get_rid(child_index)->page_no != child->get_page_no()) {
        ++child_index;
    }
    assert(child_index < page_header_->key_count);
    return child_index;
}

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int BPlusTreeNode::lower_bound(const char* target) const {
    int left = 0;
    int right = get_size();
    while (left < right) {
        const int mid = left + (right - left) / 2;
        if (compare_index_key(get_key(mid), target, file_header_->column_types_, file_header_->column_lengths_) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[0,num_key]，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 内部结点用它定位孩子时，返回 0 表示 target 比第一个 key 还小（只会出现在最左侧路径上）。
 */
int BPlusTreeNode::upper_bound(const char* target) const {
    int left = 0;
    int right = get_size();
    while (left < right) {
        const int mid = left + (right - left) / 2;
        if (compare_index_key(get_key(mid), target, file_header_->column_types_, file_header_->column_lengths_) <= 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool BPlusTreeNode::leaf_lookup(const char* key, Rid** value) {
    if (value == nullptr) {
        throw InternalError("leaf_lookup requires a non-null output pointer");
    }
    *value = nullptr;

    const int pos = lower_bound(key);
    if (pos >= get_size() ||
        compare_index_key(get_key(pos), key, file_header_->column_types_, file_header_->column_lengths_) != 0) {
        return false;
    }

    *value = get_rid(pos);
    return true;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t BPlusTreeNode::internal_lookup(const char* key) {
    if (get_size() <= 0) {
        throw InternalError("cannot search an empty internal B+ tree node");
    }

    // The key at child index i is the minimum key of child i. Select the last
    // child whose minimum is <= key; a new minimum belongs to the first child.
    const int upper = upper_bound(key);
    const int child_idx = upper == 0 ? 0 : upper - 1;
    return value_at(child_idx);
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param key 连续键数组的起始地址。
 * @param rid 连续 Rid 数组的起始地址。
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 *                            key_slot
 *                            /      \
 *                           /        \
 *       [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 *                      key           key_slot
 */
void BPlusTreeNode::insert_pairs(int pos, const char* key, const Rid* rid, int n) {
    const int old_size = get_size();
    if (pos < 0 || pos > old_size || n < 0 || old_size + n > get_max_size()) {
        throw InternalError("Invalid B+ tree node insert range");
    }
    if (n == 0) {
        return;
    }

    const size_t key_length = static_cast<size_t>(file_header_->key_length_);
    const int tail = old_size - pos;
    // 键区和值区分开连续存放，两段都要腾出 n 个槽位。
    if (tail > 0) {
        memmove(get_key(pos + n), get_key(pos), static_cast<size_t>(tail) * key_length);
        memmove(get_rid(pos + n), get_rid(pos), static_cast<size_t>(tail) * sizeof(Rid));
    }
    memcpy(get_key(pos), key, static_cast<size_t>(n) * key_length);
    memcpy(get_rid(pos), rid, static_cast<size_t>(n) * sizeof(Rid));
    set_size(old_size + n);
}

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param key 要插入的键。
 * @param value 要插入的 Rid。
 * @return int 键值对数量
 */
int BPlusTreeNode::insert(const char* key, const Rid& value) {
    // upper_bound 保证相同 key 的新记录排在已有记录之后。
    const int pos = upper_bound(key);
    insert_pairs(pos, key, &value, 1);
    return get_size();
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void BPlusTreeNode::erase_pair(int pos) {
    const int size = get_size();
    if (pos < 0 || pos >= size) {
        throw InternalError("Invalid B+ tree node erase position");
    }

    // 键区与 Rid 区分开连续存放，两段都要把 pos 之后的元素前移一格。
    const int tail = size - pos - 1;
    if (tail > 0) {
        const size_t key_length = static_cast<size_t>(file_header_->key_length_);
        memmove(get_key(pos), get_key(pos + 1), static_cast<size_t>(tail) * key_length);
        memmove(get_rid(pos), get_rid(pos + 1), static_cast<size_t>(tail) * sizeof(Rid));
    }
    set_size(size - 1);
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int BPlusTreeNode::remove(const char* key, const Rid& rid) {
    const int size = get_size();
    int pos = lower_bound(key);
    // 同一 key 可能有多条，必须连同 rid 一起匹配；只删除命中的那一条。
    while (pos < size && compare_index_key(get_key(pos), key, file_header_->column_types_,
                                           file_header_->column_lengths_) == 0) {
        const Rid* candidate = get_rid(pos);
        if (candidate->page_no == rid.page_no && candidate->slot_no == rid.slot_no) {
            erase_pair(pos);
            return get_size();
        }
        ++pos;
    }
    return size;
}

BPlusTree::BPlusTree(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager, int file_descriptor)
    : disk_manager_(disk_manager),
      buffer_pool_manager_(buffer_pool_manager),
      file_descriptor_(file_descriptor),
      file_header_() {
    alignas(int) char buf[PAGE_SIZE]{};
    disk_manager_->read_page(file_descriptor_, INDEX_FILE_HEADER_PAGE, buf, PAGE_SIZE);
    auto file_header = std::make_unique<IndexFileHeader>();
    file_header->deserialize(buf);

    // page_count_ is not a physical high-water mark after deletion or a crash.
    // Never reuse a page that is already present in the file.
    int64_t file_size = disk_manager_->get_file_size(file_descriptor_);
    if (file_size < 0) {
        throw InternalError("BPlusTree: cannot stat index file");
    }
    int64_t physical_pages_64 = file_size / PAGE_SIZE + (file_size % PAGE_SIZE != 0);
    if (physical_pages_64 > std::numeric_limits<page_id_t>::max()) {
        throw InternalError("BPlusTree: index file is too large");
    }
    auto physical_pages = static_cast<page_id_t>(physical_pages_64);
    disk_manager_->set_fd2pageno(file_descriptor_, std::max(file_header->page_count_, physical_pages));
    file_header_ = std::move(file_header);
}

/**
 * @brief 用于查找指定键所在的叶子结点
 * @param key 要查找的目标key值
 * @param operation 查找到目标键值对后要进行的操作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @param find_first 是否从叶子链表的最左端开始查找
 * @return 目标叶子结点（IndexNode 离开作用域即 unpin）以及根结点是否加锁
 */
std::pair<IndexNode, bool> BPlusTree::find_leaf_page(const char* key,
                                                     IndexOperation operation,
                                                     Transaction* transaction,
                                                     bool find_first) {
    static_cast<void>(operation);
    static_cast<void>(transaction);

    if (is_empty()) {
        return {IndexNode{}, false};
    }

    IndexNode node = fetch_node(file_header_->root_page_);
    if (!node.valid()) {
        throw InternalError("failed to fetch B+ tree root");
    }

    while (!node->is_leaf_page()) {
        const page_id_t child_page = node->internal_lookup(key);
        IndexNode child = fetch_node(child_page);
        if (!child.valid()) {
            throw InternalError("failed to fetch B+ tree child");
        }
        node = std::move(child);
    }

    if (find_first) {
        while (node->get_prev_leaf() != INDEX_LEAF_HEADER_PAGE && node->get_prev_leaf() != INDEX_NO_PAGE) {
            IndexNode previous = fetch_node(node->get_prev_leaf());
            if (!previous.valid()) {
                throw InternalError("failed to follow B+ tree leaf links");
            }
            node = std::move(previous);
        }
    }

    return {std::move(node), false};
}

/**
 * @brief 查找指定 key 对应的全部 Rid。
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 * @note 非唯一索引中，同一 key 的记录可能因分裂分布在多个相邻叶子里。
 */
bool BPlusTree::get_value(const char* key, std::vector<Rid>* result, Transaction* transaction) {
    if (result == nullptr) {
        throw InternalError("get_value requires a non-null result vector");
    }

    auto [leaf, root_is_latched] = find_leaf_page(key, IndexOperation::Find, transaction);
    static_cast<void>(root_is_latched);
    if (!leaf.valid()) {
        return false;
    }

    // Repeated keys may cross several leaves. Walk back to the first leaf in
    // the duplicate run, then scan forward until the first key greater than key.
    while (leaf->get_prev_leaf() != INDEX_LEAF_HEADER_PAGE && leaf->get_prev_leaf() != INDEX_NO_PAGE) {
        IndexNode previous = fetch_node(leaf->get_prev_leaf());
        if (!previous.valid()) {
            throw InternalError("failed to follow B+ tree leaf links");
        }
        if (previous->get_size() > 0 &&
            compare_index_key(previous->get_key(previous->get_size() - 1), key, file_header_->column_types_,
                              file_header_->column_lengths_) < 0) {
            break;
        }
        leaf = std::move(previous);
    }

    bool found = false;
    while (true) {
        const int size = leaf->get_size();
        int pos = leaf->lower_bound(key);
        while (pos < size && compare_index_key(leaf->get_key(pos), key, file_header_->column_types_,
                                               file_header_->column_lengths_) == 0) {
            result->push_back(*leaf->get_rid(pos));
            found = true;
            ++pos;
        }

        if (pos < size || leaf->get_page_no() == file_header_->last_leaf_ ||
            leaf->get_next_leaf() == INDEX_LEAF_HEADER_PAGE || leaf->get_next_leaf() == INDEX_NO_PAGE) {
            break;
        }

        IndexNode next = fetch_node(leaf->get_next_leaf());
        if (!next.valid()) {
            throw InternalError("failed to follow B+ tree leaf links");
        }
        leaf = std::move(next);
    }

    return found;
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note 返回的 IndexNode 拥有新页的 pin；原 node 仍由调用方持有。
 */
IndexNode BPlusTree::split(IndexNode& node) {
    IndexNode new_node = create_node();
    if (!new_node.valid()) {
        throw InternalError("failed to allocate B+ tree node during split");
    }

    const int old_size = node->get_size();
    const int left_size = old_size / 2;
    const int right_size = old_size - left_size;
    const bool is_leaf = node->is_leaf_page();

    // 新页内容全零，页头里不能取默认值的字段必须显式初始化。
    new_node->page_header_->is_leaf = is_leaf;
    new_node->set_parent_page_no(node->get_parent_page_no());
    new_node->set_prev_leaf(INDEX_NO_PAGE);
    new_node->set_next_leaf(INDEX_NO_PAGE);
    new_node->set_size(0);

    new_node->insert_pairs(0, node->get_key(left_size), node->get_rid(left_size), right_size);
    node->set_size(left_size);
    node.mark_dirty();
    new_node.mark_dirty();

    if (is_leaf) {
        const page_id_t new_page_no = new_node->get_page_no();
        const page_id_t old_next = node->get_next_leaf();
        new_node->set_prev_leaf(node->get_page_no());
        new_node->set_next_leaf(old_next);
        node->set_next_leaf(new_page_no);

        if (old_next == INDEX_LEAF_HEADER_PAGE) {
            IndexNode sentinel = fetch_node(INDEX_LEAF_HEADER_PAGE);
            if (!sentinel.valid()) {
                throw InternalError("failed to fetch B+ tree leaf sentinel during split");
            }
            sentinel->set_prev_leaf(new_page_no);
            sentinel.mark_dirty();
            file_header_->last_leaf_ = new_page_no;
        } else {
            IndexNode next = fetch_node(old_next);
            if (!next.valid()) {
                throw InternalError("failed to fetch B+ tree next leaf during split");
            }
            next->set_prev_leaf(new_page_no);
            next.mark_dirty();
        }
    } else {
        for (int i = 0; i < new_node->get_size(); ++i) {
            update_child_parent(new_node, i);
        }
    }

    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param old_node 分裂后保留左半部分的原结点。
 * @param key 要插入parent的key
 * @param new_node 分裂产生的右兄弟结点。
 * @param transaction 当前事务；不需要并发控制时可为 nullptr。
 * @note 一个结点插入了键值对之后需要分裂，分裂后左半部分的键值对保留在原结点，在参数中称为old_node，
 * 右半部分的键值对分裂为新的右兄弟节点，在参数中称为new_node（参考Split函数来理解old_node和new_node）
 * @note old_node / new_node 的 pin 仍由调用方的 IndexNode 持有。
 */
void BPlusTree::insert_into_parent(IndexNode& old_node,
                                   const char* key,
                                   IndexNode& new_node,
                                   Transaction* transaction) {
    static_cast<void>(transaction);

    if (old_node->is_root_page()) {
        IndexNode new_root = create_node();
        if (!new_root.valid()) {
            throw InternalError("failed to allocate B+ tree root during split");
        }
        new_root->page_header_->is_leaf = false;
        new_root->set_parent_page_no(INDEX_NO_PAGE);
        new_root->set_prev_leaf(INDEX_NO_PAGE);
        new_root->set_next_leaf(INDEX_NO_PAGE);
        new_root->set_size(0);

        // 内部结点的每个 key 都是对应孩子子树的最小值。
        Rid left_child = {.page_no = old_node->get_page_no(), .slot_no = 0};
        new_root->insert_pairs(0, old_node->get_key(0), &left_child, 1);
        Rid right_child = {.page_no = new_node->get_page_no(), .slot_no = 0};
        new_root->insert_pairs(1, key, &right_child, 1);

        set_root_page(new_root->get_page_no());
        update_child_parent(new_root, 0);
        update_child_parent(new_root, 1);
        new_root.mark_dirty();
        return;
    }

    IndexNode parent = fetch_node(old_node->get_parent_page_no());
    if (!parent.valid()) {
        throw InternalError("failed to fetch B+ tree parent during split");
    }

    const int rank = parent->find_child(&old_node.node());
    Rid child = {.page_no = new_node->get_page_no(), .slot_no = 0};
    parent->insert_pairs(rank + 1, key, &child, 1);
    parent.mark_dirty();

    if (parent->get_size() == parent->get_max_size()) {
        IndexNode new_parent = split(parent);
        insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction);
    }
}

/**
 * @brief 将指定键值对插入到B+树中
 * @param key 要插入的键。
 * @param value 键对应的 Rid。
 * @param transaction 事务指针
 * @return page_id_t 插入到的叶结点的page_no
 * @throws DuplicateKeyError 唯一索引中已存在相同 key，此时树保持不变。
 * @pre 调用方不会重复插入完全相同的 (key, value)。
 */
page_id_t BPlusTree::insert_entry(const char* key, const Rid& value, Transaction* transaction) {
    // 在任何修改之前判重，保证抛异常时树保持不变、也不遗留 pin。
    if (is_unique()) {
        std::vector<Rid> existing;
        if (get_value(key, &existing, transaction)) {
            throw DuplicateKeyError();
        }
    }

    auto [leaf, root_is_latched] = find_leaf_page(key, IndexOperation::Insert, transaction);
    static_cast<void>(root_is_latched);
    if (!leaf.valid()) {
        throw InternalError("failed to fetch B+ tree leaf for insertion");
    }

    const page_id_t leaf_page = leaf->get_page_no();
    leaf->insert(key, value);
    leaf.mark_dirty();

    if (leaf->get_size() == leaf->get_max_size()) {
        IndexNode new_leaf = split(leaf);
        insert_into_parent(leaf, new_leaf->get_key(0), new_leaf, transaction);
    }

    // 新 key 可能成为某个子树的新最小值，分隔键要沿父链一直更新到根。
    update_ancestor_keys(leaf);
    return leaf_page;
}

/**
 * @brief 删除 B+ 树中的一条 (key, rid)。
 * @param key 要删除的key值
 * @param rid 要删除的记录位置；同一 key 的其他记录保留
 * @param transaction 事务指针
 * @return 该 (key, rid) 存在并被删除时返回 true
 */
bool BPlusTree::delete_entry(const char* key, const Rid& rid, Transaction* transaction) {
    if (is_empty()) {
        return false;
    }

    auto [leaf, root_is_latched] = find_leaf_page(key, IndexOperation::Delete, transaction);
    static_cast<void>(root_is_latched);
    if (!leaf.valid()) {
        return false;
    }

    // 重复 key 会跨相邻叶子分布，向下查找可能落在这一串叶子的右边某个叶子上。
    // 先回退到这段重复 key 的第一个叶子，再向右扫到真正存放 (key, rid) 的那一个。
    while (leaf->get_prev_leaf() != INDEX_LEAF_HEADER_PAGE && leaf->get_prev_leaf() != INDEX_NO_PAGE) {
        IndexNode previous = fetch_node(leaf->get_prev_leaf());
        if (!previous.valid()) {
            throw InternalError("failed to follow B+ tree leaf links");
        }
        if (previous->get_size() > 0 &&
            compare_index_key(previous->get_key(previous->get_size() - 1), key, file_header_->column_types_,
                              file_header_->column_lengths_) < 0) {
            break;
        }
        leaf = std::move(previous);
    }

    bool found = false;
    int removed_pos = -1;
    while (true) {
        const int size = leaf->get_size();
        int pos = leaf->lower_bound(key);
        while (pos < size && compare_index_key(leaf->get_key(pos), key, file_header_->column_types_,
                                               file_header_->column_lengths_) == 0) {
            const Rid* candidate = leaf->get_rid(pos);
            if (candidate->page_no == rid.page_no && candidate->slot_no == rid.slot_no) {
                found = true;
                removed_pos = pos;
                break;
            }
            ++pos;
        }
        if (found) {
            leaf->erase_pair(removed_pos);
            leaf.mark_dirty();
            break;
        }
        // 当前叶子里 key 已经越过目标值，说明整棵树中不存在这条记录。
        if (pos < size || leaf->get_page_no() == file_header_->last_leaf_ ||
            leaf->get_next_leaf() == INDEX_LEAF_HEADER_PAGE || leaf->get_next_leaf() == INDEX_NO_PAGE) {
            break;
        }

        IndexNode next = fetch_node(leaf->get_next_leaf());
        if (!next.valid()) {
            throw InternalError("failed to follow B+ tree leaf links");
        }
        leaf = std::move(next);
    }

    if (!found) {
        return false;
    }

    // 删掉的是本结点最小值时，父链上的分隔键要立即跟着更新（结点变空时交给合并处理）。
    if (removed_pos == 0 && leaf->get_size() > 0) {
        update_ancestor_keys(leaf);
    }

    coalesce_or_redistribute(leaf, transaction, nullptr);
    return true;
}

/**
 * @brief 用于处理合并和重分配的逻辑，用于删除键值对后调用
 *
 * @param node 执行完删除操作的结点
 * @param transaction 事务指针
 * @param root_is_latched 传出参数：根节点是否上锁，用于并发操作
 * @return 是否需要删除结点
 * @note User needs to first find the sibling of input page.
 * If sibling's size + input page's size >= 2 * page's minsize, then redistribute.
 * Otherwise, merge(Coalesce).
 */
bool BPlusTree::coalesce_or_redistribute(IndexNode& node, Transaction* transaction, bool* root_is_latched) {
    // 根结点没有兄弟：下溢只能靠降低树高来处理。
    if (node->is_root_page()) {
        return adjust_root(node);
    }

    // 仍达到半满，无需调整。
    if (node->get_size() >= node->get_min_size()) {
        return false;
    }

    IndexNode parent = fetch_node(node->get_parent_page_no());
    if (!parent.valid()) {
        throw InternalError("failed to fetch B+ tree parent during rebalance");
    }

    const int index = parent->find_child(&node.node());
    // 优先借前驱兄弟；node 本身就是最左孩子时只能借后继。
    const int neighbor_index = (index == 0) ? 1 : index - 1;
    IndexNode neighbor = fetch_node(parent->value_at(neighbor_index));
    if (!neighbor.valid()) {
        throw InternalError("failed to fetch B+ tree sibling during rebalance");
    }

    // 两个结点的键值对之和够分给两个半满结点，就重分配；否则合并。
    if (node->get_size() + neighbor->get_size() >= node->get_min_size() * 2) {
        redistribute(neighbor, node, parent, index);
        return false;
    }

    coalesce(neighbor, node, parent, index, transaction, root_is_latched);
    return true;
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 * @param old_root_node 原根节点
 * @return bool 根结点是否需要被删除
 * @note size of root page can be less than min size and this method is only called within coalesce_or_redistribute()
 */
bool BPlusTree::adjust_root(IndexNode& old_root_node) {
    // 内部根只剩一个孩子：孩子上移成为新根，树高减一。
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        const page_id_t child_page = old_root_node->value_at(0);
        IndexNode child = fetch_node(child_page);
        if (!child.valid()) {
            throw InternalError("failed to fetch B+ tree child while shrinking the root");
        }
        child->set_parent_page_no(INDEX_NO_PAGE);
        child.mark_dirty();
        set_root_page(child_page);
        record_page_deletion();
        return true;
    }

    // 根叶子被删空：保留这个空叶根，而不是把根置为 INDEX_NO_PAGE。
    // 框架的插入路径由 find_leaf_page 定位叶子，它假定根结点始终存在；若把根清空，
    // 之后第一次插入就会找不到叶子。因此这里只把叶子链表退回"只剩这个空根"。
    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        const page_id_t root_page = old_root_node->get_page_no();
        file_header_->first_leaf_ = root_page;
        file_header_->last_leaf_ = root_page;
        old_root_node->set_prev_leaf(INDEX_LEAF_HEADER_PAGE);
        old_root_node->set_next_leaf(INDEX_LEAF_HEADER_PAGE);
        old_root_node.mark_dirty();

        IndexNode sentinel = fetch_node(INDEX_LEAF_HEADER_PAGE);
        if (sentinel.valid()) {
            sentinel->set_prev_leaf(root_page);
            sentinel->set_next_leaf(root_page);
            sentinel.mark_dirty();
        }
        // 根结点仍然有效，没有页面被移出树。
        return false;
    }

    // 其余情况根结点仍然合法（叶根可以少于半满）。
    return false;
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 * Redistribute key & value pairs from one page to its sibling page. If index == 0, move sibling page's first key
 * & value pair into end of input "node", otherwise move sibling page's last key & value pair into head of input "node".
 *
 * @param neighbor_node sibling page of input "node"
 * @param node input from method coalesceOrRedistribute()
 * @param parent the parent of "node" and "neighbor_node"
 * @param index node在parent中的rid_idx
 * @note node是之前刚被删除过一个key的结点
 * index=0，则neighbor是node后继结点，表示：node(left)      neighbor(right)
 * index>0，则neighbor是node前驱结点，表示：neighbor(left)  node(right)
 * 注意更新parent结点的相关kv对
 */
void BPlusTree::redistribute(IndexNode& neighbor_node, IndexNode& node, IndexNode& parent, int index) {
    // index > 0：neighbor 是 node 的前驱；index == 0：neighbor 是 node 的后继。
    if (index > 0) {
        // 左兄弟的最后一个键值对挪到 node 的最前面
        const int from = neighbor_node->get_size() - 1;
        node->insert_pairs(0, neighbor_node->get_key(from), neighbor_node->get_rid(from), 1);
        neighbor_node->erase_pair(from);
        if (!node->is_leaf_page()) {
            update_child_parent(node, 0);
        }
        // node 的最小值变了，父结点中它对应的分隔键要跟着改
        parent->set_key(index, node->get_key(0));
    } else {
        // 右兄弟的第一个键值对挪到 node 的最后面
        const int to = node->get_size();
        node->insert_pairs(to, neighbor_node->get_key(0), neighbor_node->get_rid(0), 1);
        neighbor_node->erase_pair(0);
        if (!node->is_leaf_page()) {
            update_child_parent(node, to);
        }
        // 右兄弟的最小值变了，父结点中它对应的分隔键要跟着改
        parent->set_key(index + 1, neighbor_node->get_key(0));
    }

    node.mark_dirty();
    neighbor_node.mark_dirty();
    parent.mark_dirty();

    // 父结点自身的第一个 key 也可能变了，继续向祖先修正。
    if (parent->get_size() > 0) {
        update_ancestor_keys(parent);
    }
}

/**
 * @brief 合并(Coalesce)函数是将node和其直接前驱进行合并，也就是和它左边的neighbor_node进行合并；
 * 假设node一定在右边。如果上层传入的index=0，说明node在左边，那么交换node和neighbor_node，保证node在右边；合并到左结点，实际上就是删除了右结点；
 * Move all the key & value pairs from one page to its sibling page, and notify buffer pool manager to delete this page.
 * Parent page must be adjusted to take info of deletion into account. Remember to deal with coalesce or redistribute
 * recursively if necessary.
 *
 * @param neighbor_node sibling page of input "node" (neighbor_node是node的前结点)
 * @param node input from method coalesceOrRedistribute() (node结点是需要被删除的)
 * @param parent parent page of input "node"
 * @param index node在parent中的rid_idx
 * @param transaction 当前事务；不需要并发控制时可为 nullptr。
 * @param root_is_latched 传出根结点的加锁状态。
 * @return true means parent node should be deleted, false means no deletion happend
 * @note Assume that *neighbor_node is the left sibling of *node (neighbor -> node)
 */
bool BPlusTree::coalesce(IndexNode& neighbor_node,
                         IndexNode& node,
                         IndexNode& parent,
                         int index,
                         Transaction* transaction,
                         bool* root_is_latched) {
    // 统一成"左结点吸收右结点"：index == 0 时 node 在左，否则 neighbor 在左。
    IndexNode* left = (index == 0) ? &node : &neighbor_node;
    IndexNode* right = (index == 0) ? &neighbor_node : &node;
    const int left_index = (index == 0) ? 0 : index - 1;
    const int right_index = (index == 0) ? 1 : index;

    const int left_size = (*left)->get_size();
    const int right_size = (*right)->get_size();
    const bool is_leaf = (*left)->is_leaf_page();

    // 1. 把右结点的键值对整体接到左结点末尾（搬的是连续数组，一次搞定）。
    if (right_size > 0) {
        (*left)->insert_pairs(left_size, (*right)->get_key(0), (*right)->get_rid(0), right_size);
    }
    (*left).mark_dirty();

    if (is_leaf) {
        // 2a. 叶结点：右结点被删除，先把叶子链表接好，再修正首尾叶子。
        if (file_header_->last_leaf_ == (*right)->get_page_no()) {
            file_header_->last_leaf_ = (*left)->get_page_no();
        }
        if (file_header_->first_leaf_ == (*right)->get_page_no()) {
            file_header_->first_leaf_ = (*left)->get_page_no();
        }
        unlink_leaf(*right);
    } else {
        // 2b. 内部结点：搬过来的孩子要改认左结点为父。
        for (int i = left_size; i < (*left)->get_size(); ++i) {
            update_child_parent(*left, i);
        }
    }

    // 3. 父结点里删掉右结点对应的键值对；左结点若曾是空结点，分隔键要一并修正。
    if ((*left)->get_size() > 0) {
        parent->set_key(left_index, (*left)->get_key(0));
    }
    parent->erase_pair(right_index);
    parent.mark_dirty();
    record_page_deletion();

    if (parent->get_size() > 0) {
        update_ancestor_keys(parent);
    }

    // 4. 父结点可能也下溢，继续向上；父结点是根时由 adjust_root 降低树高。
    coalesce_or_redistribute(parent, transaction, root_is_latched);
    return true;
}

/**
 * @brief 将索引扫描位置转换为表记录位置 Rid。
 *
 * @param position 叶子页号和页内槽号。
 * @return 对应索引项保存的 Rid。
 * @note IndexPosition 描述索引内部位置，Rid 描述表记录位置，二者含义不同。
 */
Rid BPlusTree::get_rid(const IndexPosition& position) const {
    IndexNode node = fetch_node(position.page_no);
    if (!node.valid() || position.slot_no < 0 || position.slot_no >= node->get_size()) {
        throw IndexEntryNotFoundError();
    }
    return *node->get_rid(position.slot_no);
}

/**
 * @brief 返回整棵树中第一个 key >= 目标 key 的索引项位置。
 *
 * @param key 目标 key
 * @return IndexPosition 可作为 IndexScan 的起点
 * @note 位置规范：若结果恰好落在某个非最后叶子的末尾（slot_no == size），应改为下一叶子的
 *       第 0 个槽；若所有 key 都小于目标 key，返回 leaf_end()。这样得到的位置才能与
 *       IndexScan::next() 产生的位置直接比较。
 */
IndexPosition BPlusTree::lower_bound(const char* key) {
    auto [leaf, root_is_latched] = find_leaf_page(key, IndexOperation::Find, nullptr);
    static_cast<void>(root_is_latched);
    if (!leaf.valid()) {
        return IndexPosition{.page_no = INDEX_NO_PAGE, .slot_no = -1};
    }

    // Routing may choose the rightmost child when equal keys span children.
    // Move back until the previous leaf no longer contains the target key.
    while (leaf->get_prev_leaf() != INDEX_LEAF_HEADER_PAGE && leaf->get_prev_leaf() != INDEX_NO_PAGE) {
        IndexNode previous = fetch_node(leaf->get_prev_leaf());
        if (!previous.valid()) {
            throw InternalError("failed to follow B+ tree leaf links");
        }
        if (previous->get_size() > 0 &&
            compare_index_key(previous->get_key(previous->get_size() - 1), key, file_header_->column_types_,
                              file_header_->column_lengths_) < 0) {
            break;
        }
        leaf = std::move(previous);
    }

    while (true) {
        const int pos = leaf->lower_bound(key);
        if (pos < leaf->get_size()) {
            return {.page_no = leaf->get_page_no(), .slot_no = pos};
        }
        if (leaf->get_page_no() == file_header_->last_leaf_ || leaf->get_next_leaf() == INDEX_LEAF_HEADER_PAGE ||
            leaf->get_next_leaf() == INDEX_NO_PAGE) {
            return leaf_end();
        }

        IndexNode next = fetch_node(leaf->get_next_leaf());
        if (!next.valid()) {
            throw InternalError("failed to follow B+ tree leaf links");
        }
        leaf = std::move(next);
    }
}

/**
 * @brief 返回整棵树中第一个 key > 目标 key 的索引项位置。
 *
 * @param key 目标 key
 * @return IndexPosition 可作为 IndexScan 的终点
 * @note 位置规范与 lower_bound() 相同。
 */
IndexPosition BPlusTree::upper_bound(const char* key) {
    auto [leaf, root_is_latched] = find_leaf_page(key, IndexOperation::Find, nullptr);
    static_cast<void>(root_is_latched);
    if (!leaf.valid()) {
        return IndexPosition{.page_no = INDEX_NO_PAGE, .slot_no = -1};
    }

    while (true) {
        const int pos = leaf->upper_bound(key);
        if (pos < leaf->get_size()) {
            return {.page_no = leaf->get_page_no(), .slot_no = pos};
        }
        if (leaf->get_page_no() == file_header_->last_leaf_ || leaf->get_next_leaf() == INDEX_LEAF_HEADER_PAGE ||
            leaf->get_next_leaf() == INDEX_NO_PAGE) {
            return leaf_end();
        }

        IndexNode next = fetch_node(leaf->get_next_leaf());
        if (!next.valid()) {
            throw InternalError("failed to follow B+ tree leaf links");
        }
        leaf = std::move(next);
    }
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 * 用处在于可以作为IndexScan的最后一个
 *
 * @return IndexPosition
 */
IndexPosition BPlusTree::leaf_end() const {
    IndexNode node = fetch_node(file_header_->last_leaf_);
    if (!node.valid()) {
        throw InternalError("failed to fetch last leaf for index scan");
    }
    return {.page_no = file_header_->last_leaf_, .slot_no = node->get_size()};
}

/**
 * @brief 指向第一个叶子的第一个结点
 * 用处在于可以作为IndexScan的第一个
 *
 * @return IndexPosition
 */
IndexPosition BPlusTree::leaf_begin() const { return {.page_no = file_header_->first_leaf_, .slot_no = 0}; }

IndexNode BPlusTree::fetch_node(page_id_t page_no) const {
    PageGuard guard = buffer_pool_manager_->fetch_page_guard(PageId{.fd = file_descriptor_, .page_no = page_no});
    return {file_header_.get(), std::move(guard)};
}

IndexNode BPlusTree::create_node() {
    PageId new_page_id = {.fd = file_descriptor_, .page_no = INVALID_PAGE_ID};
    PageGuard guard = buffer_pool_manager_->new_page_guard(&new_page_id);
    if (!guard) {
        return {};
    }
    file_header_->page_count_++;
    return {file_header_.get(), std::move(guard)};
}

void BPlusTree::update_ancestor_keys(IndexNode& node) {
    page_id_t current_page = node->get_page_no();
    while (true) {
        IndexNode current = fetch_node(current_page);
        if (!current.valid() || current->get_parent_page_no() == INDEX_NO_PAGE) {
            break;
        }
        IndexNode parent = fetch_node(current->get_parent_page_no());
        if (!parent.valid()) {
            break;
        }
        const int rank = parent->find_child(&current.node());
        if (memcmp(parent->get_key(rank), current->get_key(0), file_header_->key_length_) == 0) {
            break;
        }
        parent->set_key(rank, current->get_key(0));
        parent.mark_dirty();
        current_page = parent->get_page_no();
    }
}

// 框架辅助：调用即可。这段失败回滚是框架内部实现，作业不要模仿。
void BPlusTree::unlink_leaf(IndexNode& leaf) {
    if (!leaf.valid() || !leaf->is_leaf_page()) {
        throw InternalError("unlink_leaf requires a valid leaf node");
    }

    const page_id_t leaf_page = leaf->get_page_no();
    const page_id_t prev_page = leaf->get_prev_leaf();
    const page_id_t next_page = leaf->get_next_leaf();

    {
        IndexNode prev = fetch_node(prev_page);
        if (!prev.valid()) {
            throw InternalError("unlink_leaf failed to fetch previous leaf");
        }
        prev->set_next_leaf(next_page);
        prev.mark_dirty();
    }
    {
        IndexNode next = fetch_node(next_page);
        if (!next.valid()) {
            IndexNode prev = fetch_node(prev_page);
            if (prev.valid()) {
                prev->set_next_leaf(leaf_page);
                prev.mark_dirty();
            }
            throw InternalError("unlink_leaf failed to fetch next leaf; previous link rolled back");
        }
        next->set_prev_leaf(prev_page);
        next.mark_dirty();
    }
}

void BPlusTree::record_page_deletion() { file_header_->page_count_--; }

void BPlusTree::update_child_parent(IndexNode& node, int child_idx) {
    if (!node.valid() || node->is_leaf_page()) {
        return;
    }
    IndexNode child = fetch_node(node->value_at(child_idx));
    if (!child.valid()) {
        return;
    }
    child->set_parent_page_no(node->get_page_no());
    child.mark_dirty();
}
