#include "../../../include/Optimize/Scalar/DemandDrivenCopyProjection.h"
#include "../../../include/Optimize/Analysis/AffineCopySummary.h"
#include "../../../include/Optimize/Analysis/Dominators.h"
#include "../../../include/Optimize/Analysis/LoopInfo.h"
#include "../../../include/Optimize/Loop/LoopUtils/LoopTripUtils.h"
#include "../../../include/IR/Instruction.h"
#include <set>

using namespace sysy;

namespace {

struct Address { Value* base = nullptr; Value* index = nullptr; };
static Address address(Value* pointer) {
    auto* gep = dyn_cast<GetElementPtrInst>(pointer);
    if (!gep) return {};
    // Keep the exact pointer value used by the caller.  In particular, an
    // array-to-pointer decay is a meaningful part of the call interface: the
    // callee and the post-call demand both use that decayed pointer.
    return {gep->getOperand(0), gep->getOperand(1)};
}

static PhiInst* loopIV(Loop* loop) {
    for (auto* inst : loop->head->getInstructions()) {
        auto* phi = dyn_cast<PhiInst>(inst); if (!phi) break;
        for (int i=0;i<phi->getNumOperands();i+=2) {
            auto* bb=dyn_cast<BasicBlock>(phi->getOperand(i+1));
            auto* n=dyn_cast<BinaryInst>(phi->getOperand(i));
            if (bb && loop->has(bb) && n && n->getOperand(0)==phi &&
                (n->getOpID()==Instruction::Add||n->getOpID()==Instruction::Sub) &&
                isa<ConstantInt>(n->getOperand(1))) return phi;
        }
    }
    return nullptr;
}

static Value* invariantBound(Loop* loop, SCEV& scev) {
    ExitBranchInfo info;
    if (!analyzeExitBranch(loop, loop->head, scev, info)) return nullptr;
    if (info.lhsRec && info.lhsRec->loop==loop && isLoopInvariantValue(info.rhs,loop)) return info.rhs;
    if (info.rhsRec && info.rhsRec->loop==loop && isLoopInvariantValue(info.lhs,loop)) return info.lhs;
    return nullptr;
}

static Function* makeTraceFunction(Module* module, Type* rowsType) {
    std::string name="__copy_projection_trace";
    int suffix=0; while(module->getFunction(name)) name="__copy_projection_trace_"+std::to_string(++suffix);
    auto* f=new Function(name,Type::getIntTy()); module->addFunction(f);
    auto* target=new Argument(Type::getIntTy(),"target",f,0);
    auto* n=new Argument(Type::getIntTy(),"extent",f,1);
    auto* len=new Argument(Type::getIntTy(),"count",f,2);
    auto* rows=new Argument(rowsType,"parameters",f,3);
    for(auto* a:{target,n,len,rows}) f->addArgument(a);
    auto* entry=new BasicBlock("trace.entry",f->getBody());
    auto* r0=new BinaryInst(Instruction::Sub,len,new ConstantInt(1),entry);
    auto* head=new BasicBlock("trace.calls",f->getBody()); new BranchInst(head,entry);
    auto* r=new PhiInst(Type::getIntTy(),head); r->addIncoming(r0,entry);
    auto* x=new PhiInst(Type::getIntTy(),head); x->addIncoming(target,entry);
    auto* more=new ICmpInst(ICmpInst::SGE,r,new ConstantInt(0),head);
    auto* setup=new BasicBlock("trace.setup",f->getBody());
    auto* done=new BasicBlock("trace.done",f->getBody()); new BranchInst(more,setup,done,head);
    auto* rp=new GetElementPtrInst(rows,r,setup); auto* row=new LoadInst(rp,setup);
    auto* col=new BinaryInst(Instruction::Div,n,row,setup);
    auto* nonzero=new ICmpInst(ICmpInst::SGT,col,new ConstantInt(0),setup);
    auto* local=new BasicBlock("trace.local",f->getBody());
    auto* next=new BasicBlock("trace.next",f->getBody()); new BranchInst(nonzero,local,next,setup);
    auto* lx=new PhiInst(Type::getIntTy(),local); lx->addIncoming(x,setup);
    auto* ci=new PhiInst(Type::getIntTy(),local); ci->addIncoming(n,setup);
    auto* cj=new PhiInst(Type::getIntTy(),local); cj->addIncoming(n,setup);
    auto* j=new BinaryInst(Instruction::Div,lx,col,local);
    auto* i=new BinaryInst(Instruction::Mod,lx,col,local);
    auto* jrow=new ICmpInst(ICmpInst::SLT,j,row,local);
    auto* ji=new ICmpInst(ICmpInst::SLE,j,i,local);
    auto* ilt=new ICmpInst(ICmpInst::SLT,i,ci,local);
    auto* ieq=new ICmpInst(ICmpInst::EQ,i,ci,local);
    auto* jlt=new ICmpInst(ICmpInst::SLT,j,cj,local);
    auto* tie=new BinaryInst(Instruction::And,ieq,jlt,local);
    auto* earlier=new BinaryInst(Instruction::Or,ilt,tie,local);
    auto* valid0=new BinaryInst(Instruction::And,jrow,ji,local);
    auto* valid=new BinaryInst(Instruction::And,valid0,earlier,local);
    auto* body=new BasicBlock("trace.follow",f->getBody()); new BranchInst(valid,body,next,local);
    auto* prod=new BinaryInst(Instruction::Mul,i,row,body);
    auto* nx=new BinaryInst(Instruction::Add,prod,j,body); new BranchInst(local,body);
    lx->addIncoming(nx,body); ci->addIncoming(i,body); cj->addIncoming(j,body);
    auto* out=new PhiInst(Type::getIntTy(),next); out->addIncoming(x,setup); out->addIncoming(lx,local);
    auto* rn=new BinaryInst(Instruction::Sub,r,new ConstantInt(1),next); new BranchInst(head,next);
    r->addIncoming(rn,next); x->addIncoming(out,next);
    new ReturnInst(x,done);
    return f;
}

static bool project(Module* module, const AffineCopySummary& copy) {
    auto getConst=[&](const LoopAffineExpr& e,PhiInst* iv,int64_t v){
        auto it=e.constantCoefficients.find(iv); return it!=e.constantCoefficients.end()&&it->second==v;
    };
    auto getSym=[&](const LoopAffineExpr& e,PhiInst* iv)->Value*{
        auto it=e.symbolicCoefficients.find(iv); return it==e.symbolicCoefficients.end()?nullptr:it->second;
    };
    Value* row=getSym(copy.sourceIndex,copy.outerIV);
    Value* col=getSym(copy.destinationIndex,copy.innerIV);
    auto exact=[&](const LoopAffineExpr& e){
        return e.constant==0 && e.invariantTerms.empty() &&
               e.constantCoefficients.size()==1 &&
               e.symbolicCoefficients.size()==1;
    };
    if(!row||!col||!getConst(copy.sourceIndex,copy.innerIV,1)||
       !getConst(copy.destinationIndex,copy.outerIV,1)||
       !exact(copy.sourceIndex)||!exact(copy.destinationIndex)||
       row!=copy.innerLimit||col!=copy.outerBound) return false;
    auto* rowArg=dyn_cast<Argument>(row); auto* div=dyn_cast<BinaryInst>(col);
    if(!rowArg||!div||div->getOpID()!=Instruction::Div||div->getOperand(1)!=rowArg) return false;
    auto* extentArg=dyn_cast<Argument>(div->getOperand(0)); if(!extentArg) return false;

    CallInst* site=nullptr; Function* caller=nullptr;
    for(auto* f:module->getFunctions()) for(auto* bb:f->getBody()->getBlocks()) for(auto* inst:bb->getInstructions())
        if(auto* c=dyn_cast<CallInst>(inst); c&&c->getFunction()==copy.function){if(site)return false;site=c;caller=f;}
    if(!site||!caller) return false;
    Dominators dt(caller);dt.run(); LoopInfo li(caller,dt); SCEV scev(caller,li);
    Loop* calls=li.loopOf(site->getParent()); if(!calls||calls->sub.size())return false;
    PhiInst* civ=loopIV(calls); Value* len=invariantBound(calls,scev); if(!civ||!len)return false;
    Value* rowActual=site->getOperand(rowArg->getArgNo()+1);
    auto* rowLoad=dyn_cast<LoadInst>(rowActual); if(!rowLoad)return false;
    Address ra=address(rowLoad->getOperand(0)); if(!ra.base||ra.index!=civ)return false;
    Value* matrix=site->getOperand(cast<Argument>(copy.base)->getArgNo()+1);
    Value* extent=site->getOperand(extentArg->getArgNo()+1);

    std::vector<LoadInst*> demands;
    for(auto* bb:caller->getBody()->getBlocks()) for(auto* inst:bb->getInstructions()) if(auto* ld=dyn_cast<LoadInst>(inst)){
        Address a=address(ld->getOperand(0)); if(a.base!=matrix)continue;
        if(calls->has(bb))return false;
        if(!dt.dominates(calls->exits[0],bb))continue;
        demands.push_back(ld);
    }
    if(demands.empty())return false;
    auto* trace=makeTraceFunction(module,ra.base->getType());
    for(auto* ld:demands){Address a=address(ld->getOperand(0));auto* call=new CallInst(trace,{a.index,extent,len,ra.base},nullptr);
        call->setParent(ld->getParent());auto& is=ld->getParent()->getInstructions();
        auto* gep=cast<GetElementPtrInst>(ld->getOperand(0));
        // The projected index feeds the GEP, so it must precede the GEP in
        // instruction order as well as in the SSA graph.
        auto p=std::find(is.begin(),is.end(),gep);if(p==is.end())return false;is.insert(p,call);
        gep->setOperand(1,call);}
    site->eraseInst();
    return true;
}
} // namespace

bool DemandDrivenCopyProjection::run(){
    std::vector<Function*> fs=M->getFunctions();
    for(auto* f:fs){AffineCopySummary s;if(AffineCopyAnalysis(f).run(s)&&project(M,s))return true;}
    return false;
}
