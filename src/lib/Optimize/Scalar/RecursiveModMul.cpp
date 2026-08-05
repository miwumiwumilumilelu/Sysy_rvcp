#include "../../../include/Optimize/Scalar/RecursiveModMul.h"
#include "../../../include/IR/Instruction.h"
#include <climits>
#include <set>

using namespace sysy;
namespace {
static ReturnInst* directReturn(BasicBlock* bb){
    if(!bb||bb->getInstructions().empty())return nullptr;
    return dyn_cast<ReturnInst>(bb->getInstructions().back());
}
static bool branchesTo(ICmpInst* cmp,Value* yes,Value* no){
    auto* f=cmp->getParent()->getParentFunc();
    for(auto* bb:f->getBody()->getBlocks())for(auto* i:bb->getInstructions())if(auto* br=dyn_cast<BranchInst>(i);br&&br->getNumOperands()==3&&br->getOperand(0)==cmp){
        auto* rt=directReturn(dyn_cast<BasicBlock>(br->getOperand(1)));
        auto* rf=directReturn(dyn_cast<BasicBlock>(br->getOperand(2)));
        if(rt&&rf&&rt->getOperand(0)==yes&&rf->getOperand(0)==no)return true;
    }return false;
}
static bool branchTrueTo(ICmpInst* cmp,Value* yes){
    auto* f=cmp->getParent()->getParentFunc();
    for(auto* bb:f->getBody()->getBlocks())for(auto* i:bb->getInstructions())if(auto* br=dyn_cast<BranchInst>(i);br&&br->getNumOperands()==3&&br->getOperand(0)==cmp){
        auto* r=directReturn(dyn_cast<BasicBlock>(br->getOperand(1)));
        if(r&&r->getOperand(0)==yes)return true;
    }return false;
}
static ConstantInt* samePositiveMod(Value* v,Value* lhs){
    auto* mod=dyn_cast<BinaryInst>(v);if(!mod||mod->getOpID()!=Instruction::Mod||mod->getOperand(0)!=lhs)return nullptr;
    auto* c=dyn_cast<ConstantInt>(mod->getOperand(1));
    return c&&c->getValue()>0&&c->getValue()<=INT_MAX/2?c:nullptr;
}
static bool transform(Function* f){
    if(f->getArgs().size()!=2||!f->getType()->isInt())return false;
    Value* a=f->getArgs()[0];Value* b=f->getArgs()[1];CallInst* rec=nullptr;
    for(auto* bb:f->getBody()->getBlocks())for(auto* i:bb->getInstructions()){
        if(auto* c=dyn_cast<CallInst>(i);c&&c->getFunction()==f){if(rec)return false;rec=c;}
    }
    if(!rec||rec->getOperand(1)!=a)return false;
    auto* half=dyn_cast<BinaryInst>(rec->getOperand(2));auto* two=half?dyn_cast<ConstantInt>(half->getOperand(1)):nullptr;
    if(!half||half->getOpID()!=Instruction::Div||half->getOperand(0)!=b||!two||two->getValue()!=2)return false;
    BinaryInst *twice=nullptr,*even=nullptr,*oddAdd=nullptr,*odd=nullptr;ConstantInt* modulus=nullptr;
    for(auto* bb:f->getBody()->getBlocks())for(auto* i:bb->getInstructions())
        if(auto* x=dyn_cast<BinaryInst>(i);x&&x->getOpID()==Instruction::Add&&x->getOperand(0)==rec&&x->getOperand(1)==rec)twice=x;
    if(!twice)return false;
    for(auto* bb:f->getBody()->getBlocks())for(auto* i:bb->getInstructions())
        if(auto* x=dyn_cast<BinaryInst>(i);x&&x->getOpID()==Instruction::Mod&&x->getOperand(0)==twice)even=x;
    if(!even||(modulus=dyn_cast<ConstantInt>(even->getOperand(1)))==nullptr||modulus->getValue()<=0||modulus->getValue()>INT_MAX/2)return false;
    for(auto* bb:f->getBody()->getBlocks())for(auto* i:bb->getInstructions())if(auto* x=dyn_cast<BinaryInst>(i);x&&x->getOpID()==Instruction::Add&&
       ((x->getOperand(0)==even&&x->getOperand(1)==a)||(x->getOperand(1)==even&&x->getOperand(0)==a)))oddAdd=x;
    if(!oddAdd)return false;
    for(auto* bb:f->getBody()->getBlocks())for(auto* i:bb->getInstructions())if(auto* x=dyn_cast<BinaryInst>(i);x&&x->getOpID()==Instruction::Mod&&x->getOperand(0)==oddAdd){
        auto* mc=dyn_cast<ConstantInt>(x->getOperand(1));if(mc&&mc->getValue()==modulus->getValue())odd=x;
    }
    if(!odd)return false;
    Value* baseOne=nullptr;bool hasZero=false;
    for(auto* bb:f->getBody()->getBlocks())if(auto* r=directReturn(bb)){
        if(auto* z=dyn_cast<ConstantInt>(r->getOperand(0));z&&z->getValue()==0)hasZero=true;
        auto* mc=samePositiveMod(r->getOperand(0),a);
        if(mc&&mc->getValue()==modulus->getValue())baseOne=r->getOperand(0);
    }
    if(!hasZero||!baseOne)return false;
    ICmpInst *c0=nullptr,*c1=nullptr,*co=nullptr;
    for(auto* bb:f->getBody()->getBlocks())for(auto* i:bb->getInstructions())if(auto* c=dyn_cast<ICmpInst>(i)){
        auto* k=dyn_cast<ConstantInt>(c->getOperand(1));
        if(c->getPredicate()==ICmpInst::EQ&&c->getOperand(0)==b&&k&&k->getValue()==0)c0=c;
        if(c->getPredicate()==ICmpInst::EQ&&c->getOperand(0)==b&&k&&k->getValue()==1)c1=c;
        auto* rem=dyn_cast<BinaryInst>(c->getOperand(0));
        if(c->getPredicate()==ICmpInst::EQ&&rem&&rem->getOpID()==Instruction::Mod&&rem->getOperand(0)==b&&
           isa<ConstantInt>(rem->getOperand(1))&&cast<ConstantInt>(rem->getOperand(1))->getValue()==2&&k&&k->getValue()==1)co=c;
    }
    if(!c0||!c1||!co)return false;
    bool zeroBranch=false;
    for(auto* bb:f->getBody()->getBlocks())for(auto* i:bb->getInstructions())if(auto* br=dyn_cast<BranchInst>(i);br&&br->getNumOperands()==3&&br->getOperand(0)==c0){
        auto* r=directReturn(dyn_cast<BasicBlock>(br->getOperand(1)));
        auto* z=r?dyn_cast<ConstantInt>(r->getOperand(0)):nullptr;
        zeroBranch|=z&&z->getValue()==0;
    }
    if(!zeroBranch)return false;
    if(!branchTrueTo(c1,baseOne)||!branchesTo(co,odd,even))return false;
    auto* old=f->getEntryBlock();auto* guard=new BasicBlock("modmul.guard",f->getBody());
    f->getBody()->getBlocks().remove(guard);f->getBody()->getBlocks().push_front(guard);
    auto* fast=new BasicBlock("modmul.fast",f->getBody());
    auto* an=new ICmpInst(ICmpInst::SGE,a,new ConstantInt(0),guard);auto* bn=new ICmpInst(ICmpInst::SGE,b,new ConstantInt(0),guard);
    auto* alt=new ICmpInst(ICmpInst::SLT,a,modulus,guard);auto* both=new BinaryInst(Instruction::And,an,bn,guard);
    auto* safe=new BinaryInst(Instruction::And,both,alt,guard);new BranchInst(safe,fast,old,guard);
    auto* wide=new FastModMulInst(a,b,modulus,fast);new ReturnInst(wide,fast);
    return true;
}
}
bool RecursiveModMul::run(){bool c=false;for(auto* f:M->getFunctions())c|=transform(f);return c;}
