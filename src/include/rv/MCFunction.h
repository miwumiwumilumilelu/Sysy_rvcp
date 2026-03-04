#ifndef MCFUNCTION_H
#define MCFUNCTION_H

#include "rv/MCBlock.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_set>

namespace sysy {
namespace rv {

class MCFunction {
public:
    std::string name;
    std::vector<std::unique_ptr<MCBlock>> blocks;

    VReg nextVReg = 1;

    struct VRegInfo {
        VReg vreg;
        bool isFloat;
        MCOperand* defOp = nullptr;          
        std::vector<MCOperand*> uses;         
        Reg preg = Reg::zero;             
        int spillOffset = -1; // -1 is not spilled.
        bool isSpilled = false;      

        VRegInfo() : vreg(0), isFloat(false) {}
        VRegInfo(VReg v, bool f) : vreg(v), isFloat(f) {}
    };

    // Index access for vregInfo is O(1).
    std::vector<VRegInfo> vregInfo;

    // Stack argument passing metadata.
    struct StackArgInfo {
        VReg vreg;     // VReg holding the incoming/outgoing value
        int  slotIdx;  // overflow slot index (0 = first overflow arg, in declaration order)
        bool isFloat;  // true → flw/fsw, false → lw/sw
    };

    // Callee: register-passed args (first 8 int → a0..a7, first 8 float → fa0..fa7).
    // RegAlloc pre-colors these VRegs to the corresponding physical registers.
    std::vector<VReg> args;
    std::vector<bool> argIsFloat;

    // Callee: stack-passed incoming args beyond the 8-register limit.
    // Prologue pass emits: lw/flw vreg, (frameSize + slotIdx*4)(sp)
    std::vector<StackArgInfo> incomingStackArgs;

    int frameSize = 0;
    // No Call
    bool isLeaf = true;
    static constexpr int StackAlign = 16;

    explicit MCFunction(std::string n) : name(std::move(n)) {
        // Pre-allocated to avoid frequent reallocation.
        vregInfo.reserve(256);
        vregInfo.emplace_back(); // Start from 1, so vregInfo[0] is invalid.
    }

    MCBlock* createBlock(std::string name) {
        auto block = std::make_unique<MCBlock>(std::move(name), this);
        blocks.push_back(std::move(block));
        return blocks.back().get();
    }

    MCBlock* getEntryBlock() {
        return blocks.empty() ? nullptr : blocks.front().get();
    }

    VReg newVReg(bool isFloat = false) {
        VReg vreg = nextVReg++;
        if (vregInfo.size() <= vreg) {
            vregInfo.resize(vreg + 128);
        }
        vregInfo[vreg] = VRegInfo(vreg, isFloat);
        return vreg;
    }

    void setVRegDef(VReg vreg, MCOperand* def) {
        if (vreg < vregInfo.size()) {
            vregInfo[vreg].defOp = def;
        }
    }

    void addVRegUse(VReg vreg, MCOperand* use) {
        if (vreg < vregInfo.size()) {
            vregInfo[vreg].uses.push_back(use);
        }
    }

    VRegInfo* getVRegInfo(VReg vreg) {
        if (vreg > 0 && vreg < vregInfo.size() && vregInfo[vreg].vreg == vreg) {
            return &vregInfo[vreg];
        }
        return nullptr;
    }

    const VRegInfo* getVRegInfo(VReg vreg) const {
        if (vreg > 0 && vreg < vregInfo.size() && vregInfo[vreg].vreg == vreg) {
            return &vregInfo[vreg];
        }
        return nullptr;
    }

    void setPReg(VReg vreg, Reg preg) {
        if (auto* info = getVRegInfo(vreg)) {
            info->preg = preg;
        }
    }

    void setSpillOffset(VReg vreg, int offset) {
        if (auto* info = getVRegInfo(vreg)) {
            info->spillOffset = offset;
            info->isSpilled = true;
        }
    }

    template<typename F>
    void forEachInst(F&& f) {
        for (auto& block : blocks) {
            for (RvOp* op = block->head; op; op = op->next) {
                f(op);
            }
        }
    }

    template<typename F>
    void forEachInst(F&& f) const {
        for (auto& block : blocks) {
            for (RvOp* op = block->head; op; op = op->next) {
                f(op);
            }
        }
    }

    void analyzeLeaf() {
        isLeaf = true;
        forEachInst([&](RvOp* op) {
            if (op->opcode == RvOp::CallOp) {
                isLeaf = false;
            }
        });
    }

    void buildDefUseChains() {
        for (auto& info : vregInfo) {
            info.defOp = nullptr;
            info.uses.clear();
        }

        std::vector<MCOperand*> usesBuf;
        usesBuf.reserve(8);

        // Build def-use chains.(vector<VregInfo>)
        forEachInst([&](RvOp* op) {
            MCOperand* def = op->getDef();
            if (def && def->isVReg()) {
                VReg vreg = def->getVReg();
                if (vreg >= vregInfo.size()) {
                    vregInfo.resize(vreg + 128);
                }
                vregInfo[vreg].defOp = def;
            }

            usesBuf.clear();
            op->collectUses(usesBuf);
            for (MCOperand* use : usesBuf) {
                if (use->isVReg()) {
                    VReg vreg = use->getVReg();
                    if (vreg >= vregInfo.size()) {
                        vregInfo.resize(vreg + 128);
                    }
                    vregInfo[vreg].uses.push_back(use);
                }
            }
        });
    }

    std::vector<VReg> getAllVRegs() const {
        std::vector<VReg> result;
        result.reserve(nextVReg - 1);
        for (size_t i = 1; i < vregInfo.size(); ++i) {
            if (vregInfo[i].vreg != 0) {
                result.push_back(static_cast<VReg>(i));
            }
        }
        return result;
    }
    std::vector<VReg> getAllFloatVRegs() const {
        std::vector<VReg> result;
        for (size_t i = 1; i < vregInfo.size(); ++i) {
            if (vregInfo[i].vreg != 0 && vregInfo[i].isFloat) {
                result.push_back(static_cast<VReg>(i));
            }
        }
        return result;
    }
    std::vector<VReg> getAllIntVRegs() const {
        std::vector<VReg> result;
        for (size_t i = 1; i < vregInfo.size(); ++i) {
            if (vregInfo[i].vreg != 0 && !vregInfo[i].isFloat) {
                result.push_back(static_cast<VReg>(i));
            }
        }
        return result;
    }

    void computeFrameSize() {
        int maxOffset = 0;
        for (const auto& info : vregInfo) {
            if (info.isSpilled && info.spillOffset > maxOffset) {
                maxOffset = info.spillOffset;
            }
        }
        frameSize = ((maxOffset + StackAlign - 1) / StackAlign) * StackAlign;
    }

    void eraseAndUpdate(RvOp* op) {
        if (!op || !op->parent) return;
        op->parent->erase(op);
        buildDefUseChains();
    }

};

} // namespace rv
} // namespace sysy

#endif
