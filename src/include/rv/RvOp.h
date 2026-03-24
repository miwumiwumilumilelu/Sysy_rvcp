#ifndef RVOP_H
#define RVOP_H

// Mimic LLVM TableGen system
/*
def ADD : RVInstR<
    0b0000000,        
    0b000,              
    OPC_OP,         
    (outs GPR:$rd),     
    (ins GPR:$rs1, GPR:$rs2), 
    "add",          
    "$rd, $rs1, $rs2"  
>;
*/

#include "rv/RvReg.h"
#include <variant>
#include <vector>
#include <string>
#include <cstdint>
#include <ostream>

namespace sysy {
namespace rv {

class MCBlock;

using VReg = uint32_t;
constexpr VReg InvalidVReg = 0;

#define RV_INSTRUCTIONS \
    X(Addi) X(Add) X(Addw) X(Subw) X(Mulw) X(Divw) X(Remw) \
    X(Sll) X(Srl) X(Sra) X(Slliw) X(Srliw) X(Sraiw) X(Andi) \
    X(And) X(Or) X(Xor) X(Slt) X(Sltu) X(Sltiu) X(Slti) \
    X(FAddS) X(FSubS) X(FMulS) X(FDivS) \
    /* call @sqrt(float %a) -> FSqrtS */ \
    /* %res = %a < %b ? %a : %b -> FMinS */ \
    X(FSqrtS) X(FMinS) X(FMaxS) \
    /* rd = (rs1 × rs2) + rs3 -> FMaddS */ \
    /* rd = (rs1 × rs2) - rs3 -> FMsubS */ \
    /* rd = -(rs1 × rs2) + rs3 -> FNmsubS */ \
    /* rd = -(rs1 × rs2) - rs3 -> FnmaddS */ \
    X(FMaddS) X(FMsubS) X(FNmsubS) X(FnmaddS) \
    X(FCvtWS) X(FCvtSW) X(FCvtLS) X(FCvtSL) \
    X(FEQS) X(FLTS) X(FLES) \
    X(Lw) X(Lh) X(Lb) X(Lwu) X(Lhu) X(Lbu) X(Ld) \
    X(Sw) X(Sh) X(Sb) X(Sd) \
    X(FLw) X(FSw) \
    X(Beq) X(Bne) X(Blt) X(Ble) X(Bgt) X(Bge) \
    X(Beqz) X(Bnez) X(Blez) X(Bgez) X(Bltz) X(Bgtz) \
    /* call @main -> JAL */ \
    /* call %ptr -> JALR */ \
    /* br label %bb2 -> J */ \
    /* jr ra -> Jr */ \
    X(J) X(Jr) X(JAL) X(JALR) \
    X(Li) X(La) X(Mv) \
    /* fmv.w.x: int reg → float reg (bit-level), used for float const materialization */ \
    /* fmv.s:   float reg → float reg move */ \
    X(FMvWX) X(FMvS) \
    X(Call) X(Ret)

class MCOperand {
    // val is Safe Union. See https://en.cppreference.com/w/cpp/utility/variant
    std::variant<std::monostate, VReg, Reg, int, std::string> val;

public:
    MCOperand() : val(std::monostate{}) {}

    // VReg is essentially uint32_t, and the imm number is int,
    // explicit: Avoid ambiguity caused by implicit conversions.
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

    bool isIntReg() const {
        return isPReg() && !isFP(getPReg());
    }
    bool isFloatReg() const {
        return isPReg() && isFP(getPReg());
    }

    bool operator==(const MCOperand& o) const { return val == o.val; }
    bool operator!=(const MCOperand& o) const { return val != o.val; }
};

inline std::ostream& operator<<(std::ostream& os, const MCOperand& op) {
    if (op.isEmpty()) return os << "invalid";
    if (op.isVReg())  return os << "%" << op.getVReg();
    if (op.isPReg())  return os << showReg(op.getPReg());
    if (op.isImm())   return os << op.getImm();
    if (op.isLabel()) return os << op.getLabel();
    return os << "?";
}

class RvOp {
public:
    enum Opcode {
#define X(name) name##Op,
        RV_INSTRUCTIONS
#undef X
    };

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

    virtual MCOperand* getDef() { return nullptr; }
    virtual void collectUses(std::vector<MCOperand*>& /*uses*/) const {}
    virtual void collectAll(std::vector<MCOperand*>& /*all*/) const {}

    void insertAfter(RvOp* op) {
        if (next) next->prev = op;
        op->next = next;
        op->prev = this;
        next = op;
        op->parent = parent;
    }

    virtual void print(std::ostream& os) const = 0;

protected:
    // Directly calling will bypass MCBlock's head/tail updates;
    // it must be done through MCBlock::remove()/erase().
    void remove() {
        if (prev) prev->next = next;
        if (next) next->prev = prev;
    }

    friend class MCBlock;
};

class RVInstI : public RvOp {
public:
    MCOperand rd, rs;
    int imm;
    const char* asmName;

    RVInstI(Opcode op, const char* name, MCOperand d, MCOperand s, int i)
        : RvOp(op), rd(d), rs(s), imm(i), asmName(name) {}

    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override { uses.push_back(const_cast<MCOperand*>(&rs)); }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs));
    }
    void print(std::ostream& os) const override {
        os << "    " << asmName << " " << rd << ", " << rs << ", " << imm << "\n";
    }
};

class AddiOp : public RVInstI { public: AddiOp(MCOperand d, MCOperand s, int i) : RVInstI(RvOp::AddiOp, "addi", d, s, i) {} };

class RVInstR : public RvOp {
public:
    MCOperand rd, rs1, rs2;
    const char* asmName;

    RVInstR(Opcode op, const char* name, MCOperand d, MCOperand s1, MCOperand s2)
        : RvOp(op), rd(d), rs1(s1), rs2(s2), asmName(name) {}

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

    void print(std::ostream& os) const override {
        os << "    " << asmName << " " << rd << ", " << rs1 << ", " << rs2 << "\n";
    }
};

class AddwOp  : public RVInstR { public: AddwOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::AddwOp, "addw", d, s1, s2) {} };
class SubwOp  : public RVInstR { public: SubwOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::SubwOp, "subw", d, s1, s2) {} };
class MulwOp  : public RVInstR { public: MulwOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::MulwOp, "mulw", d, s1, s2) {} };
class DivwOp  : public RVInstR { public: DivwOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::DivwOp, "divw", d, s1, s2) {} };
class RemwOp  : public RVInstR { public: RemwOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::RemwOp, "remw", d, s1, s2) {} };
class AndOp   : public RVInstR { public: AndOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::AndOp, "and", d, s1, s2) {} };
class OrOp    : public RVInstR { public: OrOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::OrOp, "or", d, s1, s2) {} };
class XorOp   : public RVInstR { public: XorOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::XorOp, "xor", d, s1, s2) {} };
class AddOp   : public RVInstR { public: AddOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::AddOp, "add", d, s1, s2) {} };
class SllOp   : public RVInstR { public: SllOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::SllOp, "sll", d, s1, s2) {} };
class SrlOp   : public RVInstR { public: SrlOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::SrlOp, "srl", d, s1, s2) {} };
class SraOp   : public RVInstR { public: SraOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::SraOp, "sra", d, s1, s2) {} };
class SltOp   : public RVInstR { public: SltOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::SltOp, "slt", d, s1, s2) {} };
class SltuOp  : public RVInstR { public: SltuOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::SltuOp, "sltu", d, s1, s2) {} };
class SltiuOp : public RVInstI { public: SltiuOp(MCOperand d, MCOperand s, int i) : RVInstI(RvOp::SltiuOp, "sltiu", d, s, i) {} };
class SltiOp  : public RVInstI { public: SltiOp (MCOperand d, MCOperand s, int i) : RVInstI(RvOp::SltiOp,  "slti",  d, s, i) {} };
class SlliwOp : public RVInstI { public: SlliwOp(MCOperand d, MCOperand s, int i) : RVInstI(RvOp::SlliwOp, "slliw", d, s, i) {} };
class SrliwOp : public RVInstI { public: SrliwOp(MCOperand d, MCOperand s, int i) : RVInstI(RvOp::SrliwOp, "srliw", d, s, i) {} };
class SraiwOp : public RVInstI { public: SraiwOp(MCOperand d, MCOperand s, int i) : RVInstI(RvOp::SraiwOp, "sraiw", d, s, i) {} };
class AndiOp  : public RVInstI { public: AndiOp (MCOperand d, MCOperand s, int i) : RVInstI(RvOp::AndiOp,  "andi",  d, s, i) {} };
class FAddSOp : public RVInstR { public: FAddSOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::FAddSOp, "fadd.s", d, s1, s2) {} };
class FSubSOp : public RVInstR { public: FSubSOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::FSubSOp, "fsub.s", d, s1, s2) {} };
class FMulSOp : public RVInstR { public: FMulSOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::FMulSOp, "fmul.s", d, s1, s2) {} };
class FDivSOp : public RVInstR { public: FDivSOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::FDivSOp, "fdiv.s", d, s1, s2) {} };
class FEQSOp  : public RVInstR { public: FEQSOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::FEQSOp, "feq.s", d, s1, s2) {} };
class FLTSOp  : public RVInstR { public: FLTSOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::FLTSOp, "flt.s", d, s1, s2) {} };
class FLESOp  : public RVInstR { public: FLESOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::FLESOp, "fle.s", d, s1, s2) {} };
class FMinSOp : public RVInstR { public: FMinSOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::FMinSOp, "fmin.s", d, s1, s2) {} };
class FMaxSOp : public RVInstR { public: FMaxSOp(MCOperand d, MCOperand s1, MCOperand s2) : RVInstR(RvOp::FMaxSOp, "fmax.s", d, s1, s2) {} };

class RVInstM : public RvOp {
public:
    MCOperand reg, base;
    int offset;
    const char* asmName;
    // Check if need Def.
    bool isStore;

    RVInstM(Opcode op, const char* name, MCOperand r, MCOperand b, int o, bool store)
        : RvOp(op), reg(r), base(b), offset(o), asmName(name), isStore(store) {}

    MCOperand* getDef() override { return isStore ? nullptr : &reg; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&base));
        if (isStore) uses.push_back(const_cast<MCOperand*>(&reg));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&reg));
        all.push_back(const_cast<MCOperand*>(&base));
    }
    void print(std::ostream& os) const override {
        os << "    " << asmName << " " << reg << ", " << offset << "(" << base << ")\n";
    }
};

class LwOp  : public RVInstM { public: LwOp(MCOperand d, MCOperand b, int o) : RVInstM(RvOp::LwOp, "lw", d, b, o, false) {} };
class SwOp  : public RVInstM { public: SwOp(MCOperand s, MCOperand b, int o) : RVInstM(RvOp::SwOp, "sw", s, b, o, true) {} };
class LdOp  : public RVInstM { public: LdOp(MCOperand d, MCOperand b, int o) : RVInstM(RvOp::LdOp, "ld", d, b, o, false) {} };
class SdOp  : public RVInstM { public: SdOp(MCOperand s, MCOperand b, int o) : RVInstM(RvOp::SdOp, "sd", s, b, o, true) {} };
class FLwOp : public RVInstM { public: FLwOp(MCOperand d, MCOperand b, int o) : RVInstM(RvOp::FLwOp, "flw", d, b, o, false) {} };
class FSwOp : public RVInstM { public: FSwOp(MCOperand s, MCOperand b, int o) : RVInstM(RvOp::FSwOp, "fsw", s, b, o, true) {} };

class RVInstU : public RvOp {
public:
    MCOperand rd, rs;
    const char* asmName;

    RVInstU(Opcode op, const char* name, MCOperand d, MCOperand s)
        : RvOp(op), rd(d), rs(s), asmName(name) {}

    MCOperand* getDef() override { return &rd; }
    void collectUses(std::vector<MCOperand*>& uses) const override { uses.push_back(const_cast<MCOperand*>(&rs)); }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs));
    }
    void print(std::ostream& os) const override {
        os << "    " << asmName << " " << rd << ", " << rs << "\n";
    }
};

class MvOp     : public RVInstU { public: MvOp(MCOperand d, MCOperand s) : RVInstU(RvOp::MvOp, "mv", d, s) {} };
class FCvtWSOp : public RVInstU {
public:
    FCvtWSOp(MCOperand d, MCOperand s) : RVInstU(RvOp::FCvtWSOp, "fcvt.w.s", d, s) {}
    // fptosi always truncates toward zero; override print to emit rtz.
    void print(std::ostream& os) const override {
        os << "    fcvt.w.s " << rd << ", " << rs << ", rtz\n";
    }
};
class FCvtSWOp : public RVInstU { public: FCvtSWOp(MCOperand d, MCOperand s) : RVInstU(RvOp::FCvtSWOp, "fcvt.s.w", d, s) {} };
class FSqrtSOp : public RVInstU { public: FSqrtSOp(MCOperand d, MCOperand s) : RVInstU(RvOp::FSqrtSOp, "fsqrt.s", d, s) {} };
class FMvWXOp  : public RVInstU { public: FMvWXOp(MCOperand d, MCOperand s) : RVInstU(RvOp::FMvWXOp, "fmv.w.x", d, s) {} };
class FMvSOp   : public RVInstU { public: FMvSOp(MCOperand d, MCOperand s)  : RVInstU(RvOp::FMvSOp,  "fmv.s",   d, s) {} };

class RVInstB : public RvOp {
public:
    MCOperand rs1, rs2;
    std::string target;
    const char* asmName;

    RVInstB(Opcode op, const char* name, MCOperand s1, MCOperand s2, std::string t)
        : RvOp(op), rs1(s1), rs2(s2), target(std::move(t)), asmName(name) {}

    MCOperand* getDef() override { return nullptr; }
    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
    }
    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
    }
    void print(std::ostream& os) const override {
        os << "    " << asmName << " " << rs1 << ", " << rs2 << ", " << target << "\n";
    }
};

class BeqOp : public RVInstB { public: BeqOp(MCOperand s1, MCOperand s2, std::string t) : RVInstB(RvOp::BeqOp, "beq", s1, s2, std::move(t)) {} };
class BneOp : public RVInstB { public: BneOp(MCOperand s1, MCOperand s2, std::string t) : RVInstB(RvOp::BneOp, "bne", s1, s2, std::move(t)) {} };
class BeqzOp : public RVInstB { public: BeqzOp(MCOperand r, std::string t) : RVInstB(RvOp::BeqzOp, "beq", r, MCOperand(Reg::zero), std::move(t)) {} };
class BnezOp : public RVInstB { public: BnezOp(MCOperand r, std::string t) : RVInstB(RvOp::BnezOp, "bne", r, MCOperand(Reg::zero), std::move(t)) {} };
class BltOp : public RVInstB { public: BltOp(MCOperand s1, MCOperand s2, std::string t) : RVInstB(RvOp::BltOp, "blt", s1, s2, std::move(t)) {} };
class BleOp : public RVInstB { public: BleOp(MCOperand s1, MCOperand s2, std::string t) : RVInstB(RvOp::BleOp, "ble", s1, s2, std::move(t)) {} };
class BgtOp : public RVInstB { public: BgtOp(MCOperand s1, MCOperand s2, std::string t) : RVInstB(RvOp::BgtOp, "bgt", s1, s2, std::move(t)) {} };
class BgeOp : public RVInstB { public: BgeOp(MCOperand s1, MCOperand s2, std::string t) : RVInstB(RvOp::BgeOp, "bge", s1, s2, std::move(t)) {} };
class BlezOp : public RVInstB { public: BlezOp(MCOperand r, std::string t) : RVInstB(RvOp::BlezOp, "ble", r, MCOperand(Reg::zero), std::move(t)) {} };
class BgezOp : public RVInstB { public: BgezOp(MCOperand r, std::string t) : RVInstB(RvOp::BgezOp, "bge", r, MCOperand(Reg::zero), std::move(t)) {} };
class BltzOp : public RVInstB { public: BltzOp(MCOperand r, std::string t) : RVInstB(RvOp::BltzOp, "blt", r, MCOperand(Reg::zero), std::move(t)) {} };
class BgtzOp : public RVInstB { public: BgtzOp(MCOperand r, std::string t) : RVInstB(RvOp::BgtzOp, "bgt", r, MCOperand(Reg::zero), std::move(t)) {} };

class LiOp : public RvOp {
public:
    MCOperand rd; int imm;
    LiOp(MCOperand d, int i) : RvOp(RvOp::LiOp), rd(d), imm(i) {}
    MCOperand* getDef() override { return &rd; }
    void collectAll(std::vector<MCOperand*>& all) const override { all.push_back(const_cast<MCOperand*>(&rd)); }
    void print(std::ostream& os) const override { os << "    li " << rd << ", " << imm << "\n"; }
};

class LaOp : public RvOp {
public:
    MCOperand rd; std::string symbol;
    LaOp(MCOperand d, std::string sym) : RvOp(RvOp::LaOp), rd(d), symbol(std::move(sym)) {}
    MCOperand* getDef() override { return &rd; }
    void collectAll(std::vector<MCOperand*>& all) const override { all.push_back(const_cast<MCOperand*>(&rd)); }
    void print(std::ostream& os) const override { os << "    la " << rd << ", " << symbol << "\n"; }
};

class JOp : public RvOp {
public:
    std::string label;
    explicit JOp(std::string l) : RvOp(RvOp::JOp), label(std::move(l)) {}
    void print(std::ostream& os) const override { os << "    j " << label << "\n"; }
};

class JrOp : public RvOp {
public:
    MCOperand rs;
    explicit JrOp(MCOperand r) : RvOp(RvOp::JrOp), rs(r) {}
    void collectUses(std::vector<MCOperand*>& uses) const override { uses.push_back(const_cast<MCOperand*>(&rs)); }
    void collectAll(std::vector<MCOperand*>& all) const override { all.push_back(const_cast<MCOperand*>(&rs)); }
    void print(std::ostream& os) const override { os << "    jr " << rs << "\n"; }
};

class JALOp : public RvOp {
public:
    std::string target;
    explicit JALOp(std::string t) : RvOp(RvOp::JALOp), target(std::move(t)) {}
    void print(std::ostream& os) const override { os << "    jal " << target << "\n"; }
};

class JALROp : public RvOp {
public:
    MCOperand rs;
    explicit JALROp(MCOperand r) : RvOp(RvOp::JALROp), rs(r) {}
    void collectUses(std::vector<MCOperand*>& uses) const override { uses.push_back(const_cast<MCOperand*>(&rs)); }
    void collectAll(std::vector<MCOperand*>& all) const override { all.push_back(const_cast<MCOperand*>(&rs)); }
    void print(std::ostream& os) const override { os << "    jalr " << rs << "\n"; }
};

class CallOp : public RvOp {
public:
    std::string target;

    // Caller-side stack args beyond the 8-register limit.
    // Prologue/Epilogue pass emits sw/fsw before this CallOp, at sp+slotIdx*4.
    struct StackArg {
        MCOperand src;  
        int  slotIdx; 
        bool isFloat; 
    };
    std::vector<StackArg> stackArgs;

    explicit CallOp(std::string t) : RvOp(RvOp::CallOp), target(std::move(t)) {}
    void print(std::ostream& os) const override { os << "    call " << target << "\n"; }
};

class RetOp : public RvOp {
public:
    RetOp() : RvOp(RvOp::RetOp) {}
    void print(std::ostream& os) const override { os << "    ret\n"; }
};

class RVInstR4 : public RvOp {
public:
    MCOperand rd, rs1, rs2, rs3;
    const char* asmName;

    RVInstR4(Opcode op, const char* name, MCOperand d, MCOperand s1, MCOperand s2, MCOperand s3)
        : RvOp(op), rd(d), rs1(s1), rs2(s2), rs3(s3), asmName(name) {}

    MCOperand* getDef() override { return &rd; }

    void collectUses(std::vector<MCOperand*>& uses) const override {
        uses.push_back(const_cast<MCOperand*>(&rs1));
        uses.push_back(const_cast<MCOperand*>(&rs2));
        uses.push_back(const_cast<MCOperand*>(&rs3));
    }

    void collectAll(std::vector<MCOperand*>& all) const override {
        all.push_back(const_cast<MCOperand*>(&rd));
        all.push_back(const_cast<MCOperand*>(&rs1));
        all.push_back(const_cast<MCOperand*>(&rs2));
        all.push_back(const_cast<MCOperand*>(&rs3));
    }

    void print(std::ostream& os) const override {
        os << "    " << asmName << " " << rd << ", " << rs1 << ", " << rs2 << ", " << rs3 << "\n";
    }
};

class FMaddSOp  : public RVInstR4 { public: FMaddSOp(MCOperand d, MCOperand s1, MCOperand s2, MCOperand s3) : RVInstR4(RvOp::FMaddSOp, "fmadd.s", d, s1, s2, s3) {} };
class FMsubSOp  : public RVInstR4 { public: FMsubSOp(MCOperand d, MCOperand s1, MCOperand s2, MCOperand s3) : RVInstR4(RvOp::FMsubSOp, "fmsub.s", d, s1, s2, s3) {} };
class FNmsubSOp : public RVInstR4 { public: FNmsubSOp(MCOperand d, MCOperand s1, MCOperand s2, MCOperand s3) : RVInstR4(RvOp::FNmsubSOp, "fnmsub.s", d, s1, s2, s3) {} };
class FnmaddSOp : public RVInstR4 { public: FnmaddSOp(MCOperand d, MCOperand s1, MCOperand s2, MCOperand s3) : RVInstR4(RvOp::FnmaddSOp, "fnmadd.s", d, s1, s2, s3) {} };

} // namespace rv
} // namespace sysy

#endif
