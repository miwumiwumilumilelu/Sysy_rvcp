#include "../../include/rv/MCPeephole.h"
#include <unordered_map>
#include <vector>

namespace sysy {
namespace rv {

bool MCPeepholePass::isPure(RvOp* op) {
    switch (op->opcode) {
        // Store
        case RvOp::SwOp: case RvOp::ShOp: case RvOp::SbOp:
        case RvOp::SdOp: case RvOp::FSwOp:
        // Load
        case RvOp::LwOp: case RvOp::LhOp: case RvOp::LbOp:
        case RvOp::LwuOp: case RvOp::LhuOp: case RvOp::LbuOp:
        case RvOp::LdOp: case RvOp::FLwOp:
        // Control flow
        case RvOp::CallOp: case RvOp::RetOp:
        case RvOp::JOp: case RvOp::JrOp: case RvOp::JALOp: case RvOp::JALROp:
        // Branch
        case RvOp::BeqOp: case RvOp::BneOp:
        case RvOp::BltOp: case RvOp::BleOp: case RvOp::BgtOp: case RvOp::BgeOp:
        case RvOp::BeqzOp: case RvOp::BnezOp:
        case RvOp::BlezOp: case RvOp::BgezOp: case RvOp::BltzOp: case RvOp::BgtzOp:
            return false;
        default:
            // Check if the op has a def. If not, it's impure.
            return op->getDef() != nullptr;
    }
}

void MCPeepholePass::runOnBlock(MCBlock* block) {
    struct Def { 
        RvOp* op; 
        bool used; // Whether the def is used
    };
    std::unordered_map<int, Def> lastDef;
    std::vector<RvOp*> toRemove;

    // self-copy elimination & dead code elimination
    for (RvOp* op = block->head; op; op = op->next) {
        // mv rd, rd / fmv.s fd, fd 
        if (op->opcode == RvOp::MvOp || op->opcode == RvOp::FMvSOp) {
            auto* u = static_cast<RVInstU*>(op);
            if (u->rd.isReg() && u->rs.isReg() &&
                u->rd.getPReg() == u->rs.getPReg()) {
                toRemove.push_back(op);
                continue;
            }
        }

        if (!isPure(op)) {
            lastDef.clear();
            continue;
        }

        std::vector<MCOperand*> uses;
        op->collectUses(uses);
        for (auto* u : uses) {
            if (u->isPReg()) {
                auto it = lastDef.find(static_cast<int>(u->getPReg()));
                if (it != lastDef.end()) it->second.used = true;
            }
        }

        auto* def = op->getDef();
        if (def && def->isPReg()) {
            int r = static_cast<int>(def->getPReg());
            auto it = lastDef.find(r);
            if (it != lastDef.end() && !it->second.used)
                toRemove.push_back(it->second.op);
            
            lastDef[r] = {op, false};
        }
    }

    for (auto* op : toRemove)
        block->erase(op);
}

namespace {

struct StackSlot {
    Reg base;
    int offset;
    bool fp;
    int bytes;

    bool operator==(const StackSlot& o) const {
        return base == o.base && offset == o.offset && fp == o.fp && bytes == o.bytes;
    }
};

struct StackSlotHash {
    size_t operator()(const StackSlot& s) const {
        size_t h = static_cast<size_t>(s.offset);
        h ^= static_cast<size_t>(s.bytes + 0x9e3779b9 + (h << 6) + (h >> 2));
        h ^= static_cast<size_t>(static_cast<int>(s.base) + 0x9e3779b9 + (h << 6) + (h >> 2));
        h ^= static_cast<size_t>(s.fp ? 0x85ebca6b : 0xc2b2ae35);
        return h;
    }
};

static bool memoryInfo(RvOp* op, StackSlot& slot, Reg& valueReg, bool& isStore) {
    int bytes = 0;
    bool fp = false;
    switch (op->opcode) {
        case RvOp::LwOp: case RvOp::SwOp: bytes = 4; break;
        case RvOp::LdOp: case RvOp::SdOp: bytes = 8; break;
        case RvOp::FLwOp: case RvOp::FSwOp: bytes = 4; fp = true; break;
        default: return false;
    }

    auto* mem = static_cast<RVInstM*>(op);
    if (!mem->reg.isPReg() || !mem->base.isPReg())
        return false;

    slot = {mem->base.getPReg(), mem->offset, fp, bytes};
    valueReg = mem->reg.getPReg();
    isStore = mem->isStore;
    return true;
}

static void forgetReg(std::unordered_map<StackSlot, Reg, StackSlotHash>& lastStore, Reg reg) {
    for (auto it = lastStore.begin(); it != lastStore.end();) {
        if (it->second == reg) it = lastStore.erase(it);
        else ++it;
    }
}

static bool isControlOrCall(RvOp* op) {
    switch (op->opcode) {
        case RvOp::CallOp: case RvOp::RetOp:
        case RvOp::JOp: case RvOp::JrOp: case RvOp::JALOp: case RvOp::JALROp:
        case RvOp::BeqOp: case RvOp::BneOp:
        case RvOp::BltOp: case RvOp::BleOp: case RvOp::BgtOp: case RvOp::BgeOp:
        case RvOp::BeqzOp: case RvOp::BnezOp:
        case RvOp::BlezOp: case RvOp::BgezOp: case RvOp::BltzOp: case RvOp::BgtzOp:
            return true;
        default:
            return false;
    }
}

} // namespace

static void forwardStackStores(MCBlock* block) {
    std::unordered_map<StackSlot, Reg, StackSlotHash> lastStore;
    std::vector<RvOp*> ops;
    for (RvOp* op = block->head; op; op = op->next)
        ops.push_back(op);

    for (RvOp* op : ops) {
        if (!op->parent)
            continue;

        StackSlot slot;
        Reg memReg = Reg::zero;
        bool isStore = false;
        if (memoryInfo(op, slot, memReg, isStore)) {
            if (!isStore) {
                auto it = lastStore.find(slot);
                if (it != lastStore.end()) {
                    RvOp* repl = slot.fp
                        ? static_cast<RvOp*>(new FMvSOp(MCOperand(memReg), MCOperand(it->second)))
                        : static_cast<RvOp*>(new MvOp(MCOperand(memReg), MCOperand(it->second)));
                    block->replace(op, repl);
                    delete op;
                    op = repl;
                }
            } else if (slot.base == Reg::sp) {
                lastStore[slot] = memReg;
            } else {
                lastStore.clear();
            }
        } else if (isControlOrCall(op)) {
            lastStore.clear();
        } else {
            switch (op->opcode) {
                case RvOp::SwOp: case RvOp::ShOp: case RvOp::SbOp:
                case RvOp::SdOp: case RvOp::FSwOp:
                    lastStore.clear();
                    break;
                default:
                    break;
            }
        }

        if (auto* def = op->getDef()) {
            if (def->isPReg())
                forgetReg(lastStore, def->getPReg());
        }
    }
}

static void redirectBranchTarget(RvOp* op, const std::string& from, const std::string& to) {
    switch (op->opcode) {
        case RvOp::JOp:
            if (static_cast<JOp*>(op)->label == from)
                static_cast<JOp*>(op)->label = to;
            break;
        case RvOp::BeqOp: case RvOp::BneOp:
        case RvOp::BltOp: case RvOp::BleOp:
        case RvOp::BgtOp: case RvOp::BgeOp:
        case RvOp::BeqzOp: case RvOp::BnezOp:
        case RvOp::BlezOp: case RvOp::BgezOp:
        case RvOp::BltzOp: case RvOp::BgtzOp:
            if (static_cast<RVInstB*>(op)->target == from)
                static_cast<RVInstB*>(op)->target = to;
            break;
        default:
            break;
    }
}

bool MCPeepholePass::eliminateTrivialBlocks(MCFunction* func) {
    bool changed = false;
    std::vector<MCBlock*> trivial;
    for (auto& b : func->blocks) {
        if (b.get() == func->blocks.front().get()) continue;
        // Check if the bb only has one JOp.
        RvOp* only = b->head;
        if (!only || only != b->tail) continue;
        if (only->opcode != RvOp::JOp) continue;
        trivial.push_back(b.get());
    }

    for (MCBlock* triv : trivial) {
        // Self-Jump bb are not useless blocks.
        // They are part of the semantics, just skip.
        const std::string& target = static_cast<JOp*>(triv->head)->label;
        if (target == triv->name) continue;

        // Find the target block.
        MCBlock* dst = nullptr;
        for (auto& b : func->blocks)
            if (b->name == target) { dst = b.get(); break; }
        if (!dst) continue;

        for (MCBlock* pred : triv->preds) {
            pred->forEach([&](RvOp* op) {
                redirectBranchTarget(op, triv->name, target);
            });
            // Update succs.
            for (auto& s : pred->succs)
                if (s == triv) { s = dst; break; }
            // Update preds.
            if (std::find(dst->preds.begin(), dst->preds.end(), pred) == dst->preds.end())
                dst->preds.push_back(pred);
        }

        auto& dp = dst->preds;
        dp.erase(std::remove(dp.begin(), dp.end(), triv), dp.end());

        func->blocks.erase(
            std::remove_if(func->blocks.begin(), func->blocks.end(), 
                            [triv](const std::unique_ptr<MCBlock>& b) {
                                return b.get() == triv;
                            }),
            func->blocks.end()
        );

        changed = true;
    }
    return changed;
}

// bb11:
//    ...
//   j bb12;
// bb12:
//
// becomes:
//
// bb11:
//    ...
// bb12:
static bool eliminateFallthroughs(MCFunction* func) {
    bool changed = false;
    auto& blocks = func->blocks;
    for (auto it = blocks.begin(); it != blocks.end(); ++it) {
        auto next = std::next(it);
        if (next == blocks.end()) continue;
        RvOp* last = (*it)->tail;
        if (!last || last->opcode != RvOp::JOp) continue;
        if (static_cast<JOp*>(last)->label == (*next)->name) {
            (*it)->erase(last);
            changed = true;
        }
    }
    return changed;
}

void MCPeepholePass::run(MCFunction* func) {
    for (auto& b : func->blocks) {
        forwardStackStores(b.get());
        runOnBlock(b.get());
    }
    // Defend chained trampolines.
    while (eliminateTrivialBlocks(func)) {}
    eliminateFallthroughs(func);
}

} // namespace rv
} // namespace sysy
