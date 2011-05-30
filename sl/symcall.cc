/*
 * Copyright (C) 2010-2011 Kamil Dudka <kdudka@redhat.com>
 *
 * This file is part of predator.
 *
 * predator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * predator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with predator.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "symcall.hh"

#include <cl/cl_msg.hh>
#include <cl/storage.hh>

#include "symabstract.hh"
#include "symbt.hh"
#include "symcut.hh"
#include "symdebug.hh"
#include "symheap.hh"
#include "symproc.hh"
#include "symstate.hh"
#include "symutil.hh"
#include "util.hh"

#include <vector>

#include <boost/foreach.hpp>

LOCAL_DEBUG_PLOTTER(symcall, DEBUG_SYMCALL)

// /////////////////////////////////////////////////////////////////////////////
// implementation of SymCallCtx
struct SymCallCtx::Private {
    SymBackTrace                *bt;
    const CodeStorage::Fnc      *fnc;
    SymHeap                     entry;
    SymHeap                     surround;
    const struct cl_operand     *dst;
    SymStateWithJoin            rawResults;
    int                         nestLevel;
    bool                        computed;
    bool                        flushed;

    void assignReturnValue(SymHeap &sh);
    void destroyStackFrame(SymHeap &sh);

    Private(TStorRef stor):
        entry(stor),
        surround(stor)
    {
    }
};

SymCallCtx::SymCallCtx(TStorRef stor):
    d(new Private(stor))
{
    d->computed = false;
    d->flushed = false;
}

SymCallCtx::~SymCallCtx() {
    delete d;
}

bool SymCallCtx::needExec() const {
    return !d->computed;
}

const SymHeap& SymCallCtx::entry() const {
    return d->entry;
}

SymState& SymCallCtx::rawResults() {
    return d->rawResults;
}

void SymCallCtx::Private::assignReturnValue(SymHeap &sh) {
    const cl_operand &op = *this->dst;
    if (CL_OPERAND_VOID == op.code)
        // we're done for a function returning void
        return;

    // wait, we're crossing stack frame boundaries here!  We need to use one
    // backtrace instance for source operands and another one for destination
    // operands.  The called function already appears on the given backtrace, so
    // that we can get the source backtrace by removing it from there locally.
    SymBackTrace callerSiteBt(*this->bt);
    callerSiteBt.popCall();
    SymProc proc(sh, &callerSiteBt);
    proc.setLocation(&op.data.var->loc);

    const TObjId obj = proc.objByOperand(op);
    CL_BREAK_IF(OBJ_INVALID == obj);

    const TValId val = sh.valueOf(sh.objAt(VAL_ADDR_OF_RET, op.type));
    CL_BREAK_IF(VAL_INVALID == val);

    // assign the return value in the current symbolic heap
    proc.objSetValue(obj, val);
}

void SymCallCtx::Private::destroyStackFrame(SymHeap &sh) {
    SymProc proc(sh, this->bt);

    // We need to look for junk since there can be a function returning an
    // allocated object.  Then ignoring the return value on the caller's
    // side can trigger a memory leak.  See data/test-0090.c for a use case.
    proc.valDestroyTarget(VAL_ADDR_OF_RET);

    TValList live;
    sh.gatherRootObjects(live, isProgramVar);
    BOOST_FOREACH(const TValId root, live) {
        const EValueTarget code = sh.valTarget(root);
        if (VT_ON_STACK != code) {
            // not a local variable
            continue;
        }

        CVar cv(sh.cVarByRoot(root));
        if (!hasKey(this->fnc->vars, cv.uid) || cv.inst != this->nestLevel)
            // a local variable that is not here-local
            continue;

        const struct cl_loc *loc = 0;
        std::string varString = varToString(sh.stor(), cv.uid, &loc);
#if DEBUG_SE_STACK_FRAME
        CL_DEBUG_MSG(loc, "FFF destroying stack variable: " << varString);
#else
        (void) varString;
#endif
        proc.setLocation(loc);
        proc.valDestroyTarget(root);
    }
}

void joinHeapsWithCare(SymHeap &sh, SymHeap surround) {
    LDP_INIT(symcall, "join");
    LDP_PLOT(symcall, sh);
    LDP_PLOT(symcall, surround);

    TCVarList live;
    gatherProgramVars(live, sh);

    SymHeap safeSurround(sh.stor());
    splitHeapByCVars(&surround, live, &safeSurround);
    joinHeapsByCVars(&sh, &safeSurround);

    LDP_PLOT(symcall, sh);

#ifndef NDEBUG
    live.clear();
    gatherProgramVars(live, surround);
    if (!live.empty())
        CL_DEBUG("joinHeapsWithCare() did something useful");
#endif
}

void SymCallCtx::flushCallResults(SymState &dst) {
    // are we really ready for this?
    CL_BREAK_IF(d->flushed);

    // mark as done
    d->computed = true;
    d->flushed = true;

    // go through the results and make them of the form that the caller likes
    const unsigned cnt = d->rawResults.size();
    for (unsigned i = 0; i < cnt; ++i) {
        if (1 < cnt) {
            CL_DEBUG("*** SymCallCtx::flushCallResults() is processing heap #"
                     << i);
        }

        // first join the heap with its original surround
        SymHeap sh(d->rawResults[i]);
        joinHeapsWithCare(sh, d->surround);

        LDP_INIT(symcall, "post-processing");
        LDP_PLOT(symcall, sh);

        // perform all necessary action wrt. our function call convention
        d->assignReturnValue(sh);
        d->destroyStackFrame(sh);
        LDP_PLOT(symcall, sh);

#if SE_ABSTRACT_ON_CALL_DONE
        // after the final merge and cleanup, chances are that the abstraction
        // may be useful
        abstractIfNeeded(sh);
        LDP_PLOT(symcall, sh);
#endif
        // flush the result
        dst.insert(sh);
    }

    // leave backtrace
    d->bt->popCall();
}

void SymCallCtx::invalidate() {
#if SE_DISABLE_CALL_CACHE
    delete this;
#endif
}

// /////////////////////////////////////////////////////////////////////////////
// call context cache per one fnc
class PerFncCache {
    private:
        typedef std::vector<SymCallCtx *> TCtxMap;

        SymHeapUnion    huni_;
        TCtxMap         ctxMap_;

    public:
        ~PerFncCache() {
            BOOST_FOREACH(SymCallCtx *ctx, ctxMap_) {
                delete ctx;
            }
        }

        /**
         * look for the given heap; return the corresponding call ctx if found,
         * 0 otherwise
         */
        SymCallCtx* lookup(const SymHeap &sh) {
#if SE_DISABLE_CALL_CACHE
            return 0;
#endif
            int idx = huni_.lookup(sh);
            if (-1 == idx)
                return 0;

            return ctxMap_.at(idx);
        }

        /**
         * store the given heap with its corresponding call ctx into the cache
         */
        void insert(const SymHeap &sh, SymCallCtx *ctx) {
#if SE_DISABLE_CALL_CACHE
            return;
#endif
            huni_.insertNew(sh);
            ctxMap_.push_back(ctx);
            CL_BREAK_IF(huni_.size() != ctxMap_.size());
        }
};

// /////////////////////////////////////////////////////////////////////////////
// implementation of SymCallCache
struct SymCallCache::Private {
    typedef const CodeStorage::Fnc                     &TFncRef;
    typedef CodeStorage::TVarSet                        TFncVarSet;
    typedef std::map<int /* uid */, PerFncCache>        TCache;

    TCache                      cache;
    SymBackTrace                bt;

    bool rediscoverGlVar(SymHeap &sh, const CVar &cv);
    void resolveHeapCut(TCVarList &cut, SymHeap &sh, const TFncVarSet &fncVars);
    SymCallCtx* getCallCtx(const SymHeap &entry, TFncRef fnc);

    Private(TStorRef stor):
        bt(stor)
    {
    }
};

SymCallCache::SymCallCache(TStorRef stor):
    d(new Private(stor))
{
}

SymCallCache::~SymCallCache() {
    delete d;
}

SymBackTrace& SymCallCache::bt() {
    return d->bt;
}

bool SymCallCache::Private::rediscoverGlVar(SymHeap &sh, const CVar &cv) {
    const SymHeap *parent = bt.seekLastOccurrenceOfVar(cv);
    if (!parent)
        // found nowhere
        return false;

    // cut out what we need from the ancestor
    CL_DEBUG("rediscoverGlVar() is taking place...");
    SymHeap arena(*parent);
    const TCVarList cut(1, cv);
    splitHeapByCVars(&arena, cut);

    // remove the stub, we are going to replace by something useful
    const TValId at = sh.addrOfVar(cv);
    CL_BREAK_IF(isVarAlive(sh, cv));
    sh.valDestroyTarget(at);

    // now put it all together
    joinHeapsByCVars(&sh, &arena);
    return true;
}

void SymCallCache::Private::resolveHeapCut(
        TCVarList                       &cut,
        SymHeap                         &sh,
        const TFncVarSet                &fncVars)
{
    TStorRef stor = sh.stor();
    const int nestLevel = bt.countOccurrencesOfTopFnc();

    // start with all gl variables that are accessible from this function
    BOOST_FOREACH(const int uid, fncVars) {
        const CodeStorage::Var &var = stor.vars[uid];
        if (isOnStack(var))
            continue;

        CVar cv(uid, /* gl var */ 0);
        if (isVarAlive(sh, cv) || this->rediscoverGlVar(sh, cv))
            cut.push_back(cv);
    }

    TValList live;
    sh.gatherRootObjects(live, isProgramVar);
    BOOST_FOREACH(const TValId root, live) {
        const EValueTarget code = sh.valTarget(root);
        if (VT_STATIC == code)
            continue;

        CVar cv(sh.cVarByRoot(root));
        if (hasKey(fncVars, cv.uid) && cv.inst == nestLevel)
            cut.push_back(cv);
    }
}

void setCallArgs(
        SymProc                         &proc,
        const CodeStorage::Fnc          &fnc,
        const CodeStorage::Insn         &insn)
{
    // check insn validity
    using namespace CodeStorage;
    const TOperandList &opList = insn.operands;
    CL_BREAK_IF(CL_INSN_CALL != insn.code || opList.size() < 2);

    // get called fnc's args
    const TArgByPos &args = fnc.args;
    if (args.size() + 2 < opList.size()) {
        CL_DEBUG_MSG(&insn.loc,
                "too many arguments given (vararg fnc involved?)");

        CL_DEBUG_MSG(locationOf(fnc), "note: fnc was declared here");
    }

    // wait, we're crossing stack frame boundaries here!  We need to use one
    // backtrace instance for source operands and another one for destination
    // operands.  The called function already appears on the given backtrace, so
    // that we can get the source backtrace by removing it from there locally.
    const SymBackTrace &bt = *proc.bt();
    SymBackTrace callerSiteBt(bt);
    callerSiteBt.popCall();

    SymHeap &sh = proc.sh();
    SymProc srcProc(sh, &callerSiteBt);
    srcProc.setLocation(&insn.loc);

    // set args' values
    unsigned pos = /* dst + fnc */ 2;
    BOOST_FOREACH(int arg, args) {

        // cVar lookup
        const int nestLevel = bt.countOccurrencesOfFnc(uidOf(fnc));
        const CVar cVar(arg, nestLevel);
        const TValId argAddr = sh.addrOfVar(cVar);

        // object instantiation
        TStorRef stor = *fnc.stor;
        const TObjType clt = stor.vars[arg].type;
        const TObjId argObj = sh.objAt(argAddr, clt);
        CL_BREAK_IF(argObj <= 0);

        if (opList.size() <= pos) {
            // no value given for this arg
            const struct cl_loc *loc;
            std::string varString = varToString(stor, arg, &loc);
            CL_DEBUG_MSG(loc, "no fnc arg given for " << varString);
            continue;
        }

        // read the given argument's value
        const struct cl_operand &op = opList[pos++];
        const TValId val = srcProc.valFromOperand(op);
        CL_BREAK_IF(VAL_INVALID == val);

        // set the value of lhs accordingly
        proc.objSetValue(argObj, val);
    }
}

SymCallCtx* SymCallCache::Private::getCallCtx(const SymHeap &entry, TFncRef fnc) {
    // cache lookup
    const int uid = uidOf(fnc);
    PerFncCache &pfc = this->cache[uid];
    SymCallCtx *ctx = pfc.lookup(entry);
    if (!ctx) {
        // cache miss
        ctx = new SymCallCtx(entry.stor());
        ctx->d->bt      = &bt;
        ctx->d->fnc     = &fnc;
        ctx->d->entry   = entry;
        pfc.insert(entry, ctx);
        return ctx;
    }

    const struct cl_loc *loc = locationOf(fnc);

    // cache hit, perform some sanity checks
    if (!ctx->d->computed) {
        // oops, we are not ready for this!
        CL_ERROR_MSG(loc, "call cache entry found, but result not "
                "computed yet; perhaps a recursive function call?");
        return 0;
    }
    if (!ctx->d->flushed) {
        // oops, we are not ready for this!
        CL_ERROR_MSG(loc, "call cache entry found, but result not "
                "flushed yet; perhaps a recursive function call?");
        return 0;
    }

    // all OK, return the cached ctx
    return ctx;
}

SymCallCtx* SymCallCache::getCallCtx(
        SymHeap                         entry,
        const CodeStorage::Fnc          &fnc,
        const CodeStorage::Insn         &insn)
{
    const struct cl_loc *loc = &insn.loc;
    CL_DEBUG_MSG(loc, "SymCallCache is looking for " << nameOf(fnc) << "()...");

    // enlarge the backtrace
    const int uid = uidOf(fnc);
    d->bt.pushCall(uid, loc, entry);

    // create SymProc and update the location info
    SymProc proc(entry, &d->bt);
    proc.setLocation(loc);

    // check recursion depth (if any)
    const int nestLevel = d->bt.countOccurrencesOfFnc(uid);
    if (1 != nestLevel) {
        CL_WARN_MSG(loc, "support of call recursion is not stable yet");
        CL_NOTE_MSG(loc, "nestLevel is " << nestLevel);
    }

    // initialize local variables of the called fnc
    LDP_INIT(symcall, "pre-processing");
    LDP_PLOT(symcall, entry);
    setCallArgs(proc, fnc, insn);
    proc.killInsn(insn);
    LDP_PLOT(symcall, entry);

    // resolve heap cut
    TCVarList cut;
    d->resolveHeapCut(cut, entry, fnc.vars);
    LDP_PLOT(symcall, entry);

    // prune heap
    LDP_INIT(symcall, "split");
    LDP_PLOT(symcall, entry);
    SymHeap surround(entry.stor());
    splitHeapByCVars(&entry, cut, &surround);
    surround.valDestroyTarget(VAL_ADDR_OF_RET);
    LDP_PLOT(symcall, entry);
    LDP_PLOT(symcall, surround);
    
    // get either an existing ctx, or create a new one
    SymCallCtx *ctx = d->getCallCtx(entry, fnc);
    if (!ctx)
        return 0;

    // not flushed yet
    ctx->d->flushed     = false;

    // keep some properties later required to process the results
    ctx->d->dst         = &insn.operands[/* dst */ 0];
    ctx->d->nestLevel   = nestLevel;
    ctx->d->surround    = surround;
    return ctx;
}
