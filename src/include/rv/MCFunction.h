#ifndef SYSY_RV_MCFUNCTION_H
#define SYSY_RV_MCFUNCTION_H

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

    // 所有基本块（unique_ptr 管理生命周期，Cache 友好）
    std::vector<std::unique_ptr<MCBlock>> blocks;

    // 虚拟寄存器分配器
    VReg nextVReg = 1;

    // 虚拟寄存器信息结构
    struct VRegInfo {
        VReg vreg;
        bool isFloat;
        MCOperand* defOp = nullptr;           // 定义该寄存器的操作数
        std::vector<MCOperand*> uses;         // 使用该寄存器的操作数
        Reg preg = Reg::zero;                 // 分配的物理寄存器（初始为 zero）
        int spillOffset = -1;                 // 溢出到栈的偏移（-1 表示未溢出）
        bool isSpilled = false;               // 是否被溢出

        VRegInfo() : vreg(0), isFloat(false) {}
        VRegInfo(VReg v, bool f) : vreg(v), isFloat(f) {}
    };

    // 神级优化：使用 vector 代替 unordered_map
    // 由于 VReg 从 1 开始连续递增，直接用索引访问，O(1) 常数极小
    std::vector<VRegInfo> vregInfo;

    // 参数寄存器
    std::vector<VReg> args;
    std::vector<bool> argIsFloat;

    // 栈帧信息（寄存器分配后填充）
    int frameSize = 0;

    // 是否叶子函数（无 call 指令）
    bool isLeaf = true;

    // 栈对齐大小
    static constexpr int StackAlign = 16;

    explicit MCFunction(std::string n) : name(std::move(n)) {
        // 预分配 vregInfo 空间，索引 0 保留
        vregInfo.reserve(256);
        vregInfo.emplace_back();  // vregInfo[0] = 无效
    }

    // ========================================================================
    // 基本块管理
    // ========================================================================

    MCBlock* createBlock(std::string name) {
        auto block = std::make_unique<MCBlock>(std::move(name), this);
        blocks.push_back(std::move(block));
        return blocks.back().get();
    }

    MCBlock* getEntryBlock() {
        return blocks.empty() ? nullptr : blocks.front().get();
    }

    // ========================================================================
    // 虚拟寄存器管理（神级优化：数组索引代替 Hash）
    // ========================================================================

    VReg newVReg(bool isFloat = false) {
        VReg vreg = nextVReg++;
        // 确保 vregInfo 足够大
        if (vregInfo.size() <= vreg) {
            vregInfo.resize(vreg + 128);  // 预分配，减少后续扩容
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

    // 纯数组访问，无 Hash 计算
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

    // 设置物理寄存器分配
    void setPReg(VReg vreg, Reg preg) {
        if (auto* info = getVRegInfo(vreg)) {
            info->preg = preg;
        }
    }

    // 设置溢出偏移
    void setSpillOffset(VReg vreg, int offset) {
        if (auto* info = getVRegInfo(vreg)) {
            info->spillOffset = offset;
            info->isSpilled = true;
        }
    }

    // ========================================================================
    // 遍历所有指令
    // ========================================================================

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

    // ========================================================================
    // 分析
    // ========================================================================

    // 检测是否为叶子函数
    void analyzeLeaf() {
        isLeaf = true;
        forEachInst([&](RvOp* op) {
            if (op->opcode == RvOp::CallOp) {
                isLeaf = false;
            }
        });
    }

    // ========================================================================
    // Def-Use 链构建
    // 注意：此函数保存 MCOperand* 指针，如果后续 erase 指令，
    //      必须重新调用此函数，否则会有悬垂指针！
    // ========================================================================

    void buildDefUseChains() {
        // 清空旧的 def-use 信息
        // 注意：不能使用 vregInfo.clear()，因为那会把 size 变成 0，
        //       后续 vregInfo[vreg] 访问会越界！
        for (auto& info : vregInfo) {
            info.defOp = nullptr;
            info.uses.clear();
        }

        // 遍历所有指令构建 def-use 链
        std::vector<MCOperand*> usesBuf;  // 复用 buffer，避免重复分配
        usesBuf.reserve(8);

        forEachInst([&](RvOp* op) {
            // 处理 Def
            MCOperand* def = op->getDef();
            if (def && def->isVReg()) {
                VReg vreg = def->getVReg();
                // 动态扩容保护（防范直接给未分配的 vreg 赋值的极端情况）
                if (vreg >= vregInfo.size()) {
                    vregInfo.resize(vreg + 128);
                }
                vregInfo[vreg].defOp = def;
                // 推断类型（如果还没有设置）
                // TODO: 从类型系统获取
            }

            // 处理 Uses
            usesBuf.clear();
            op->collectUses(usesBuf);
            for (MCOperand* use : usesBuf) {
                if (use->isVReg()) {
                    VReg vreg = use->getVReg();
                    // 动态扩容保护
                    if (vreg >= vregInfo.size()) {
                        vregInfo.resize(vreg + 128);
                    }
                    vregInfo[vreg].uses.push_back(use);
                }
            }
        });
    }

    // 获取所有需要分配的虚拟寄存器
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

    // 获取所有浮点虚拟寄存器
    std::vector<VReg> getAllFloatVRegs() const {
        std::vector<VReg> result;
        for (size_t i = 1; i < vregInfo.size(); ++i) {
            if (vregInfo[i].vreg != 0 && vregInfo[i].isFloat) {
                result.push_back(static_cast<VReg>(i));
            }
        }
        return result;
    }

    // 获取所有整数虚拟寄存器
    std::vector<VReg> getAllIntVRegs() const {
        std::vector<VReg> result;
        for (size_t i = 1; i < vregInfo.size(); ++i) {
            if (vregInfo[i].vreg != 0 && !vregInfo[i].isFloat) {
                result.push_back(static_cast<VReg>(i));
            }
        }
        return result;
    }

    // 计算栈帧大小（根据 spillOffset）
    void computeFrameSize() {
        int maxOffset = 0;
        for (const auto& info : vregInfo) {
            if (info.isSpilled && info.spillOffset > maxOffset) {
                maxOffset = info.spillOffset;
            }
        }
        frameSize = ((maxOffset + StackAlign - 1) / StackAlign) * StackAlign;
    }

    // ========================================================================
    // 指令操作辅助
    // ========================================================================

    // 删除指令后必须重新构建 Def-Use 链
    void eraseAndUpdate(RvOp* op) {
        if (!op || !op->parent) return;
        op->parent->erase(op);
        buildDefUseChains();  // 重新构建，避免悬垂指针
    }

};

} // namespace rv
} // namespace sysy

#endif
