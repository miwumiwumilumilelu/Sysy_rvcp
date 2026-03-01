#ifndef SYSY_RV_MCBLOCK_H
#define SYSY_RV_MCBLOCK_H

#include "rv/RvOp.h"
#include <string>
#include <vector>
#include <set>
#include <unordered_set>

namespace sysy {
namespace rv {

class MCFunction;

// 基本块：维护 RvOp 的侵入式链表
class MCBlock {
public:
    std::string name;
    MCFunction* parent = nullptr;

    // 链表头尾
    RvOp* head = nullptr;
    RvOp* tail = nullptr;

    // CFG 相关
    std::vector<MCBlock*> succs;  // 后继
    std::vector<MCBlock*> preds;  // 前驱

    // 循环嵌套深度（用于寄存器分配优先级）
    int loopDepth = 0;

    // 基本块在函数中的索引（用于某些算法的稳定性）
    int index = -1;

    explicit MCBlock(std::string n, MCFunction* p = nullptr)
        : name(std::move(n)), parent(p) {}

    // ========================================================================
    // 指令管理（侵入式链表操作）- O(1) 时间复杂度
    // ========================================================================

    bool empty() const { return head == nullptr; }

    // 在链表尾部追加指令
    void append(RvOp* op) {
        if (!op) return;
        op->parent = this;
        if (!head) {
            head = tail = op;
        } else {
            tail->insertAfter(op);
            tail = op;
        }
    }

    // 在链表头部插入指令
    void prepend(RvOp* op) {
        if (!op) return;
        op->parent = this;
        if (!head) {
            head = tail = op;
        } else {
            op->next = head;
            head->prev = op;
            head = op;
        }
    }

    // 在指定指令前插入
    void insertBefore(RvOp* pos, RvOp* op) {
        if (!op) return;
        op->parent = this;
        if (!pos) {
            append(op);
            return;
        }
        op->next = pos;
        op->prev = pos->prev;
        if (pos->prev) {
            pos->prev->next = op;
        } else {
            head = op;
        }
        pos->prev = op;
    }

    // 移除指令（不删除内存）
    void remove(RvOp* op) {
        if (!op) return;
        if (op->prev) {
            op->prev->next = op->next;
        } else {
            head = op->next;
        }
        if (op->next) {
            op->next->prev = op->prev;
        } else {
            tail = op->prev;
        }
        op->parent = nullptr;
        op->prev = op->next = nullptr;
    }

    // 删除指令（释放内存）
    void erase(RvOp* op) {
        remove(op);
        delete op;
    }

    // 替换指令
    void replace(RvOp* oldOp, RvOp* newOp) {
        if (!oldOp || !newOp) return;
        newOp->parent = this;
        newOp->prev = oldOp->prev;
        newOp->next = oldOp->next;

        if (oldOp->prev) {
            oldOp->prev->next = newOp;
        } else {
            head = newOp;
        }
        if (oldOp->next) {
            oldOp->next->prev = newOp;
        } else {
            tail = newOp;
        }
    }

    // 清空所有指令
    void clear() {
        RvOp* op = head;
        while (op) {
            RvOp* next = op->next;
            delete op;
            op = next;
        }
        head = tail = nullptr;
    }

    // ========================================================================
    // 遍历接口
    // ========================================================================

    // 正向遍历回调
    template<typename F>
    void forEach(F&& f) {
        for (RvOp* op = head; op; op = op->next) {
            f(op);
        }
    }

    template<typename F>
    void forEach(F&& f) const {
        for (RvOp* op = head; op; op = op->next) {
            f(op);
        }
    }

    // 反向遍历
    template<typename F>
    void forEachReverse(F&& f) {
        for (RvOp* op = tail; op; op = op->prev) {
            f(op);
        }
    }

    // 迭代器（支持 range-for）
    class iterator {
        RvOp* current;
    public:
        iterator(RvOp* p) : current(p) {}
        RvOp* operator*() { return current; }
        iterator& operator++() { current = current->next; return *this; }
        bool operator!=(const iterator& other) const { return current != other.current; }
    };

    iterator begin() { return iterator(head); }
    iterator end() { return iterator(nullptr); }

    // ========================================================================
    // CFG 操作
    // ========================================================================

    void addSucc(MCBlock* succ) {
        if (succ) {
            succs.push_back(succ);
            succ->preds.push_back(this);
        }
    }

    bool isEntry() const {
        return preds.empty();
    }

    bool isExit() const {
        return succs.empty();
    }

    // 获取终止指令（跳转/返回）
    RvOp* getTerminator() const {
        if (tail) {
            switch (tail->opcode) {
                case RvOp::JOp:
                case RvOp::JrOp:
                case RvOp::JALOp:
                case RvOp::JALROp:
                case RvOp::CallOp:
                case RvOp::RetOp:
                case RvOp::BeqOp: case RvOp::BneOp:
                case RvOp::BltOp: case RvOp::BleOp: case RvOp::BgtOp: case RvOp::BgeOp:
                case RvOp::BeqzOp: case RvOp::BnezOp:
                case RvOp::BlezOp: case RvOp::BgezOp: case RvOp::BltzOp: case RvOp::BgtzOp:
                    return tail;
                default:
                    return nullptr;
            }
        }
        return nullptr;
    }

    // 判断是否以 Ret 指令结尾
    bool isRetBlock() const {
        return tail && tail->opcode == RvOp::RetOp;
    }

    ~MCBlock() { clear(); }
};

} // namespace rv
} // namespace sysy

#endif
