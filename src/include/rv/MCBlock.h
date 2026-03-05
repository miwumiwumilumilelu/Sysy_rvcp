#ifndef MCBLOCK_H
#define MCBLOCK_H

#include "rv/RvOp.h"
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>

namespace sysy {
namespace rv {

class MCFunction;

class MCBlock {
public:
    std::string name;
    MCFunction* parent = nullptr;

    RvOp* head = nullptr;
    RvOp* tail = nullptr;

    // CFG
    std::vector<MCBlock*> succs;
    std::vector<MCBlock*> preds;

    // Liveness sets
    std::unordered_set<VReg> liveIn;
    std::unordered_set<VReg> liveOut;

    // Used for register assignment priority.
    int loopDepth = 0;

    // Block index in function.
    int index = -1;

    explicit MCBlock(std::string n, MCFunction* p = nullptr)
        : name(std::move(n)), parent(p) {}

    bool empty() const { return head == nullptr; }

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

    void prepend(RvOp* op) {
        if (!op) return;
        op->parent = this;
        op->prev = nullptr;
        if (!head) {
            op->next = nullptr;
            head = tail = op;
        } else {
            op->next = head;
            head->prev = op;
            head = op;
        }
    }

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

    void erase(RvOp* op) {
        remove(op);
        delete op;
    }

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

    void clear() {
        RvOp* op = head;
        while (op) {
            RvOp* next = op->next;
            delete op;
            op = next;
        }
        head = tail = nullptr;
    }

    // Forward traverse
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

    // backward traverse
    template<typename F>
    void forEachReverse(F&& f) {
        for (RvOp* op = tail; op; op = op->prev) {
            f(op);
        }
    }

    // Support range-for
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

    // CFG update
    void addSucc(MCBlock* succ) {
        if (succ && std::find(succs.begin(), succs.end(), succ) == succs.end()) {
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

    RvOp* getTerminator() const {
        if (tail) {
            switch (tail->opcode) {
                case RvOp::JOp:
                case RvOp::JrOp:
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

    bool isRetBlock() const {
        return tail && tail->opcode == RvOp::RetOp;
    }

    ~MCBlock() { clear(); }
};

} // namespace rv
} // namespace sysy

#endif
