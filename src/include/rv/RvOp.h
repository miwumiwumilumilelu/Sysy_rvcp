#ifndef SYSY_RV_OP_H
#define SYSY_RV_OP_H

#include "rv/RvReg.h"
#include <variant>
#include <vector>
#include <string>
#include <cstdint>
#include <ostream>

namespace sysy {
namespace rv {

class MCBlock;

// 虚拟寄存器 ID 类型
using VReg = uint32_t;
constexpr VReg InvalidVReg = 0;

// ============================================================================
// RV32I + RV32F 指令宏定义
// ============================================================================

#define RV_INSTRUCTIONS \
    /* 整数算术 */ \
    X(Addw) X(Subw) X(Mulw) X(Divw) X(Remw) \
    X(Sll) X(Srl) X(Sra) \
    X(And) X(Or) X(Xor) X(Slt) X(Sltu) \
    /* 浮点算术 */ \
    X(FAddS) X(FSubS) X(FMulS) X(FDivS) \
    X(FSqrtS) X(FMinS) X(FMaxS) \
    X(FMaddS) X(FMsubS) X(FNmsubS) X(FnmaddS) \
    /* 类型转换 */ \
    X(FCvtWS) X(FCvtSW) X(FCvtLS) X(FCvtSL) \
    /* 浮点比较 */ \
    X(FEQS) X(FLTS) X(FLES) \
    /* 内存访问 - 整数 */ \
    X(Lw) X(Lh) X(Lb) X(Lwu) X(Lhu) X(Lbu) \
    X(Sw) X(Sh) X(Sb) \
    /* 内存访问 - 浮点 */ \
    X(FLw) X(FSw) \
    /* 分支 */ \
    X(Beq) X(Bne) X(Blt) X(Ble) X(Bgt) X(Bge) \
    X(Beqz) X(Bnez) X(Blez) X(Bgez) X(Bltz) X(Bgtz) \
    /* 跳转 */ \
    X(J) X(Jr) X(JAL) X(JALR) \
    /* 其他 */ \
    X(Li) X(La) X(Mv) \
    X(Call) X(Ret)

// 操作数：使用 variant 代替手动类型判断
class MCOperand {
    // monostate 必须放第一个，确保默认构造
    std::variant<std::monostate, VReg, Reg, int, std::string> val;

public:
    MCOperand() : val(std::monostate{}) {}

    // explicit 避免隐式转换造成的歧义
    explicit MCOperand(VReg v) : val(v) {}
    explicit MCOperand(Reg r) : val(r) {}
    explicit MCOperand(int i) : val(i) {}
    explicit MCOperand(std::string l) : val(std::move(l)) {}

    bool isEmpty()  const { return std::holds_alternative<std::monostate>(val); }
    bool isVReg()   const { return std::holds_alternative<VReg>(val); }
    bool isPReg()   const { return std::holds_alternative<Reg>(val); }
    bool isImm()    const { return std::holds_alternative<int>(val); }
    bool isLabel()  const { return std::holds_alternative<std::string>(val); }
    bool isReg()    const { return isVReg() || isPReg(); }

    VReg  getVReg()   const { return std::get<VReg>(val); }
    Reg   getPReg()   const { return std::get<Reg>(val); }
    int   getImm()    const { return std::get<int>(val); }
    const std::string& getLabel() const { return std::get<std::string>(val); }

    // 类型判断辅助
    bool isIntReg() const {
        return isPReg() && !isFP(getPReg());
    }
    bool isFloatReg() const {
        return isPReg() && isFP(getPReg());
    }
};

// 基础 Op 节点：双向链表 + 所属基本块
class RvOp {
public:
    // 使用宏生成 Opcode 枚举（避免手动维护的遗漏和冲突）
    enum Opcode {
#define X(name) name##Op,
        RV_INSTRUCTIONS
#undef X
    };

    // 获取 Opcode 名称（调试用）
    static const char* getOpcodeName(Opcode op) {
        switch (op) {
#define X(name) case name##Op: return #name;
            RV_INSTRUCTIONS
#undef X
        }
        return "Unknown";
    }

    Opcode opcode;
    RvOp *prev = nullptr;
    RvOp *next = nullptr;
    MCBlock *parent = nullptr;

    RvOp(Opcode op) : opcode(op) {}
    virtual ~RvOp() = default;

    // 数据流接口 - 寄存器分配核心
    virtual MCOperand* getDef() { return nullptr; }
    virtual void collectUses(std::vector<MCOperand*>& /*uses*/) const {}
    virtual void collectAll(std::vector<MCOperand*>& /*all*/) const {}

    // 链表操作
    void insertAfter(RvOp* op) {
        if (next) next->prev = op;
        op->next = next;
        op->prev = this;
        next = op;
        op->parent = parent;
    }

    void remove() {
        if (prev) prev->next = next;
        if (next) next->prev = prev;
    }

    virtual void print(std::ostream& os) const = 0;
};

// ============================================================================
// 整数算术指令
// ============================================================================

class AddwOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    AddwOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::AddwOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

class SubwOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    SubwOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::SubwOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

class MulwOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    MulwOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::MulwOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

class DivwOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    DivwOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::DivwOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

class RemwOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    RemwOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::RemwOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

// ============================================================================
// 浮点算术指令
// ============================================================================

class FAddSOp : public RvOp {
public:
    MCOperand fd, fs1, fs2;
    FAddSOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::FAddSOp), fd(d), fs1(s1), fs2(s2) {}
    MCOperand* getDef() override { return &fd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&fs1));
        uses.push_back(const_cast<MCOperand*>(&fs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&fd));
        all.push_back(const_cast<MCOperand*>(&fs1));
        all.push_back(const_cast<MCOperand*>(&fs2));
    }
    void print(std::ostream& os) const override;
};

class FSubSOp : public RvOp {
public:
    MCOperand fd, fs1, fs2;
    FSubSOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::FSubSOp), fd(d), fs1(s1), fs2(s2) {}
    MCOperand* getDef() override { return &fd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&fs1));
        uses.push_back(const_cast<MCOperand*>(&fs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&fd));
        all.push_back(const_cast<MCOperand*>(&fs1));
        all.push_back(const_cast<MCOperand*>(&fs2));
    }
    void print(std::ostream& os) const override;
};

class FMulSOp : public RvOp {
public:
    MCOperand fd, fs1, fs2;
    FMulSOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::FMulSOp), fd(d), fs1(s1), fs2(s2) {}
    MCOperand* getDef() override { return &fd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&fs1));
        uses.push_back(const_cast<MCOperand*>(&fs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&fd));
        all.push_back(const_cast<MCOperand*>(&fs1));
        all.push_back(const_cast<MCOperand*>(&fs2));
    }
    void print(std::ostream& os) const override;
};

class FDivSOp : public RvOp {
public:
    MCOperand fd, fs1, fs2;
    FDivSOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::FDivSOp), fd(d), fs1(s1), fs2(s2) {}
    MCOperand* getDef() override { return &fd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&fs1));
        uses.push_back(const_cast<MCOperand*>(&fs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&fd));
        all.push_back(const_cast<MCOperand*>(&fs1));
        all.push_back(const_cast<MCOperand*>(&fs2));
    }
    void print(std::ostream& os) const override;
};

// ============================================================================
// 逻辑运算指令
// ============================================================================

class AndOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    AndOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::AndOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

class OrOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    OrOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::OrOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

class XorOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    XorOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::XorOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

// ============================================================================
// 移位运算指令
// ============================================================================

class SllOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    SllOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::SllOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

class SrlOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    SrlOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::SrlOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

class SraOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    SraOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::SraOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

// ============================================================================
// 比较指令
// ============================================================================

class SltOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    SltOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::SltOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

class SltuOp : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    SltuOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::SltuOp), rd(d), rs1(s1), rs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

// ============================================================================
// 零比较分支指令
// ============================================================================

class BeqzOp : public RvOp {
public:
    MCOperand rs;
    std::string target;
    BeqzOp(MCOperand r, std::string t)
        : RvOp(RvOp::BeqzOp), rs(r), target(std::move(t)) {}
    MCOperand* getDef() override { return nullptr; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rs));
    }
    void print(std::ostream& os) const override;
};

class BnezOp : public RvOp {
public:
    MCOperand rs;
    std::string target;
    BnezOp(MCOperand r, std::string t)
        : RvOp(RvOp::BnezOp), rs(r), target(std::move(t)) {}
    MCOperand* getDef() override { return nullptr; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rs));
    }
    void print(std::ostream& os) const override;
};

// ============================================================================
// 浮点类型转换指令
// ============================================================================

class FCvtWSOp : public RvOp {
public:
    MCOperand rd;  // 整数结果
    MCOperand fs1; // 浮点源
    FCvtWSOp(MCOperand d, MCOperand s)
        : RvOp(RvOp::FCvtWSOp), rd(d), fs1(s) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&fs1));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&fs1));
    }
    void print(std::ostream& os) const override;
};

class FCvtSWOp : public RvOp {
public:
    MCOperand fd;  // 浮点结果
    MCOperand rs1; // 整数源
    FCvtSWOp(MCOperand d, MCOperand s)
        : RvOp(RvOp::FCvtSWOp), fd(d), rs1(s) {}
    MCOperand* getDef() override { return &fd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&fd));
        all.push_back(const_cast<MCOperand*>(&rs1));
    }
    void print(std::ostream& os) const override;
};

// ============================================================================
// 浮点比较指令
// ============================================================================

class FEQSOp : public RvOp {
public:
    MCOperand rd, fs1, fs2;
    FEQSOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::FEQSOp), rd(d), fs1(s1), fs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&fs1));
        uses.push_back(const_cast<MCOperand*>(&fs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&fs1));
        all.push_back(const_cast<MCOperand*>(&fs2));
    }
    void print(std::ostream& os) const override;
};

class FLTSOp : public RvOp {
public:
    MCOperand rd, fs1, fs2;
    FLTSOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::FLTSOp), rd(d), fs1(s1), fs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&fs1));
        uses.push_back(const_cast<MCOperand*>(&fs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&fs1));
        all.push_back(const_cast<MCOperand*>(&fs2));
    }
    void print(std::ostream& os) const override;
};

class FLESOp : public RvOp {
public:
    MCOperand rd, fs1, fs2;
    FLESOp(MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(RvOp::FLESOp), rd(d), fs1(s1), fs2(s2) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&fs1));
        uses.push_back(const_cast<MCOperand*>(&fs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&fs1));
        all.push_back(const_cast<MCOperand*>(&fs2));
    }
    void print(std::ostream& os) const override;
};

// ============================================================================
// 内存访问指令
// ============================================================================

class LwOp : public RvOp {
public:
    MCOperand rd, base;
    int offset;
    LwOp(MCOperand d, MCOperand b, int o) : RvOp(RvOp::LwOp), rd(d), base(b), offset(o) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&base));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&base));
    }
    void print(std::ostream& os) const override;
};

class SwOp : public RvOp {
public:
    MCOperand src, base;
    int offset;
    SwOp(MCOperand s, MCOperand b, int o) : RvOp(RvOp::SwOp), src(s), base(b), offset(o) {}
    MCOperand* getDef() override { return nullptr; }  // Store 不写寄存器
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&src));
        uses.push_back(const_cast<MCOperand*>(&base));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&src));
        all.push_back(const_cast<MCOperand*>(&base));
    }
    void print(std::ostream& os) const override;
};

class FLwOp : public RvOp {
public:
    MCOperand fd, base;
    int offset;
    FLwOp(MCOperand d, MCOperand b, int o) : RvOp(RvOp::FLwOp), fd(d), base(b), offset(o) {}
    MCOperand* getDef() override { return &fd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&base));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&fd));
        all.push_back(const_cast<MCOperand*>(&base));
    }
    void print(std::ostream& os) const override;
};

class FSwOp : public RvOp {
public:
    MCOperand fs, base;
    int offset;
    FSwOp(MCOperand s, MCOperand b, int o) : RvOp(RvOp::FSwOp), fs(s), base(b), offset(o) {}
    MCOperand* getDef() override { return nullptr; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&fs));
        uses.push_back(const_cast<MCOperand*>(&base));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&fs));
        all.push_back(const_cast<MCOperand*>(&base));
    }
    void print(std::ostream& os) const override;
};

// ============================================================================
// 分支指令
// ============================================================================

class BeqOp : public RvOp {
public:
    MCOperand rs1, rs2;
    std::string target;
    BeqOp(MCOperand s1, MCOperand s2, std::string t)
        : RvOp(RvOp::BeqOp), rs1(s1), rs2(s2), target(std::move(t)) {}
    MCOperand* getDef() override { return nullptr; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

class BneOp : public RvOp {
public:
    MCOperand rs1, rs2;
    std::string target;
    BneOp(MCOperand s1, MCOperand s2, std::string t)
        : RvOp(RvOp::BneOp), rs1(s1), rs2(s2), target(std::move(t)) {}
    MCOperand* getDef() override { return nullptr; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override;
};

// ============================================================================
// 跳转指令
// ============================================================================

class JOp : public RvOp {
public:
    std::string label;
    explicit JOp(std::string l) : RvOp(RvOp::JOp), label(std::move(l)) {}
    MCOperand* getDef() override { return nullptr; }
    void collectUses(std::vector<MCOperand*>&) const override {}  // 无寄存器操作数
    void collectAll(std::vector<MCOperand*>&) const override {}
    void print(std::ostream& os) const override;
};

class JrOp : public RvOp {
public:
    MCOperand rs;
    explicit JrOp(MCOperand r) : RvOp(RvOp::JrOp), rs(r) {}
    MCOperand* getDef() override { return nullptr; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rs));
    }
    void print(std::ostream& os) const override;
};

// ============================================================================
// 其他指令
// ============================================================================

class LiOp : public RvOp {
public:
    MCOperand rd;
    int imm;
    LiOp(MCOperand d, int i) : RvOp(RvOp::LiOp), rd(d), imm(i) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>&) const override {}
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
    }
    void print(std::ostream& os) const override;
};

class MvOp : public RvOp {
public:
    MCOperand rd, rs;
    MvOp(MCOperand d, MCOperand s) : RvOp(RvOp::MvOp), rd(d), rs(s) {}
    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs));
    }
    void print(std::ostream& os) const override;
};

class CallOp : public RvOp {
public:
    std::string target;
    explicit CallOp(std::string t) : RvOp(RvOp::CallOp), target(std::move(t)) {}
    MCOperand* getDef() override { return nullptr; }
    void collectUses(std::vector<MCOperand*>&) const override {}
    void collectAll(std::vector<MCOperand*>&) const override {}
    void print(std::ostream& os) const override;
};

class RetOp : public RvOp {
public:
    RetOp() : RvOp(RvOp::RetOp) {}
    MCOperand* getDef() override { return nullptr; }
    void collectUses(std::vector<MCOperand*>&) const override {}
    void collectAll(std::vector<MCOperand*>&) const override {}
    void print(std::ostream& os) const override;
};

} // namespace rv
} // namespace sysy

#endif
