#include "rv/PhiElim.h"
#include "rv/MCInst.h"
#include <iostream>
#include <algorithm>
#include <set>
#include <functional>

using namespace sysy;

void PhiElim::run(MCModule* m) {
    for (auto f : m->funcs) {
        runOnFunc(f);
    }
}

bool PhiElim::isFloat(int vr, MCFunc* f) {
    std::set<int> visited;
    std::function<bool(int)> dfs = [&](int curr_vr) {
        if (visited.count(curr_vr)) return false;
        visited.insert(curr_vr);

        for (auto b : f->blks) {
            for (auto i : b->insts) {
                for (size_t k = 0; k < i->ops.size(); k++) {
                    if (i->ops[k].isVReg() && i->ops[k].val == curr_vr) {
                        auto opc = i->opc;

                        bool isDef = (k == 0);
                        if (opc == MCInst::SW || opc == MCInst::FSW || opc == MCInst::BEQ || 
                            opc == MCInst::BNE || opc == MCInst::BLT || opc == MCInst::BGE || 
                            opc == MCInst::J || opc == MCInst::RET || opc == MCInst::CALL) {
                            isDef = false;
                        }

                        if (isDef) {
                            if (opc >= MCInst::FADD_S && opc <= MCInst::FDIV_S) return true;
                            if (opc == MCInst::FMV_S || opc == MCInst::FCVT_S_W || 
                                opc == MCInst::FMV_W_X || opc == MCInst::FLW) return true;

                            if (opc == MCInst::PHI) {
                                for (size_t opIdx = 1; opIdx < i->opCnt(); opIdx += 2) {
                                    if (i->getOp(opIdx).isVReg()) {
                                        if (dfs(i->getOp(opIdx).val)) return true;
                                    }
                                }
                            }
                        } else {
                            if (opc >= MCInst::FADD_S && opc <= MCInst::FDIV_S) return true;
                            if (opc == MCInst::FMV_S) return true;
                            if (opc == MCInst::FCVT_W_S || opc == MCInst::FMV_X_W) { if (k == 1) return true; }
                            if (opc == MCInst::FEQ_S || opc == MCInst::FLT_S || opc == MCInst::FLE_S) { if (k != 0) return true; }
                            if (opc == MCInst::FSW) { if (k == 0) return true; }

                            if (opc == MCInst::PHI) {
                                if (i->getOp(0).isVReg()) {
                                    if (dfs(i->getOp(0).val)) return true;
                                }
                            }
                        }
                    }
                }
            }
        }
        return false;
    };
    
    return dfs(vr);
}

std::list<MCInst*>::iterator PhiElim::findPos(MCBlk* b) {
    auto it = b->insts.end();
    while (it != b->insts.begin()) {
        auto prev = std::prev(it);
        MCInst::Opc opc = (*prev)->opc;
        if (opc == MCInst::J || opc == MCInst::BNE || opc == MCInst::BEQ ||
            opc == MCInst::BLT || opc == MCInst::BGE || opc == MCInst::BLTU || opc == MCInst::BGEU || 
            opc == MCInst::RET) {
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