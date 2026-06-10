#ifndef DOMINATORS_H
#define DOMINATORS_H

#include "../../IR/Module.h"
#include "../../IR/Value.h"
#include <map>
#include <set>
#include <vector>
#include <algorithm>

namespace sysy {

class Dominators {
public:
    explicit Dominators(Function* f) : F(f) {}

    void run();

    BasicBlock* getIDom(BasicBlock* bb) { 
        if (IDoms.find(bb) == IDoms.end()) return nullptr;
        return IDoms[bb]; 
    }

    const std::set<BasicBlock*>& getDomFrontier(BasicBlock* bb) {
        return DomFrontier[bb];
    }

    bool dominates(BasicBlock* A, BasicBlock* B);

    const std::vector<BasicBlock*>& getPredecessors(BasicBlock* bb) {
        return Predecessors[bb];
    }

    const std::vector<BasicBlock*>& getSuccessors(BasicBlock* bb) {
        return Successors[bb];
    }

private:
    Function* F;

    std::map<BasicBlock*, std::vector<BasicBlock*>> Predecessors;
    std::map<BasicBlock*, std::vector<BasicBlock*>> Successors;

    std::map<BasicBlock*, BasicBlock*> IDoms;
    std::map<BasicBlock*, std::set<BasicBlock*>> DomFrontier;

    std::map<BasicBlock*, int> PostOrderNumber;
    std::vector<BasicBlock*> PostOrder;

    void buildCFG();
    void computePostOrder(BasicBlock* bb, std::set<BasicBlock*>& visited);
    void calculateIDom();
    void calculateDomFrontier();
    
    BasicBlock* intersect(BasicBlock* b1, BasicBlock* b2);
};

}

#endif