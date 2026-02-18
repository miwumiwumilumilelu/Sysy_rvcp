#include "rv/PhiElim.h"
#include "rv/MCInst.h"
#include <iostream>
#include <algorithm>

using namespace sysy;

void PhiElim::run(MCModule* m) {
    for (auto f : m->funcs) {
        runOnFunc(f);
    }
}

bool PhiElim::isFloat(int vr, MCFunc* f) {
    for (auto b : f->blks) {
        for (auto i : b->insts) {
            for (size_t k = 0; k < i->ops.size(); k++) {
                if (i->ops[k].isVReg() && i->ops[k].val == vr) {
                    switch (i->opc) {
                        case MCInst::FADD_S:
                        case MCInst::FSUB_S:
                        case MCInst::FMUL_S:
                        case MCInst::FDIV_S:
                        case MCInst::FEQ_S:
                        case MCInst::FLT_S:
                        case MCInst::FLE_S:
                        case MCInst::FCVT_W_S:
                        case MCInst::FCVT_S_W:
                        case MCInst::FMV_X_W:
                        case MCInst::FMV_W_X:
                        case MCInst::FSW:
                        case MCInst::FMV_S:
                            return true;
                        default: break;
                    }
                }
            }
        }
    }
    return false;
}

std::list<MCInst*>::iterator PhiElim::findPos(MCBlk* b) {
    auto it = b->insts.end();
    while (it != b->insts.begin()) {
        auto prev = std::prev(it);
        MCInst::Opc opc = (*prev)->opc;
        if (opc == MCInst::J || opc == MCInst::BNE || opc == MCInst::BEQ ||
            opc == MCInst::RET || opc == MCInst::BLT || opc == MCInst::BGE) {
            it = prev;
        } else {
            break;
        }
    }
    return it;
}

void PhiElim::runOnFunc(MCFunc* f) {
    for (auto b : f->blks) {
        auto it = b->insts.begin();
        while (it != b->insts.end()) {
            MCInst* i = *it;
            if (i->opc != MCInst::PHI) {
                ++it;
                continue;
            }
            // PHI dest, val1, lbl1, val2, lbl2, ...
            MCOpnd dest = i->getOp(0);
            bool isF = isFloat(dest.val, f);

            for (size_t k = 1; k < i->opCnt(); k += 2) {
                MCOpnd val = i->getOp(k);
                MCOpnd lbl = i->getOp(k + 1);

                MCBlk* Curbb = nullptr;
                for (auto pb : f->blks) {
                    if (pb->name == lbl.label) {
                        Curbb = pb;
                        break;
                    }
                }

                if (Curbb) {
                    auto pos = findPos(Curbb);
                    MCInst* mv = new MCInst(isF ? MCInst::FMV_S : MCInst::MV);
                    mv->add(dest)->add(val);

                    mv->blk = Curbb;
                    Curbb->insts.insert(pos, mv);
                }
            }

            it = b->insts.erase(it);
        }
    }
}