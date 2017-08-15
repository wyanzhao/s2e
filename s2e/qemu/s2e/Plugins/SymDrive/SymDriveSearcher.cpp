/*
 * S2E Selective Symbolic Execution Framework
 *
 * Copyright (c) 2010, Dependable Systems Laboratory, EPFL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Dependable Systems Laboratory, EPFL nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE DEPENDABLE SYSTEMS LABORATORY, EPFL BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Currently maintained by:
 *    Vitaly Chipounov <vitaly.chipounov@epfl.ch>
 *    Volodymyr Kuznetsov <vova.kuznetsov@epfl.ch>
 *
 * All contributors are listed in S2E-AUTHORS file.
 *
 */

extern "C" {
#include "config.h"
#include "qemu-common.h"
#include "qemu-timer.h"
}

#include "SymDriveSearcher.h"
#include <llvm/Support/TimeValue.h>
#include <s2e/S2E.h>
#include <s2e/ConfigFile.h>
#include <s2e/Utils.h>
#include <s2e/S2EExecutor.h>
#include <klee/Memory.h>
#include <klee/Expr.h>

#include <iostream>

#include <llvm/Instructions.h>
#include <llvm/Constants.h>
#include <llvm/Function.h>

// A global used in softmmu_template and softmmu_defs
int g_tracing_enabled = 0;
// Constant used only in this file
static const double g_time_budget_fs = 30.0; // favor success
static const double g_time_budget_mc = 2.0; // max coverage

// #define ENABLE_INVARIANTS

namespace s2e {
namespace plugins {

static bool io_map_callback (std::string tag, void *opaque);

using namespace llvm;
using namespace klee;

#define MESSAGE_SL()                                    \
    s2e()->getMessagesStream()                          \
    << "[" << state->getID() << " / "                   \
    << s2e()->getExecutor()->getStatesCount() << "] "   \
    << " line: " << line << ".  "

#define MESSAGE_S()                                     \
    s2e()->getMessagesStream()                          \
    << "[" << state->getID() << " / "                   \
    << s2e()->getExecutor()->getStatesCount() << "] "   \

#define MESSAGE() s2e()->getMessagesStream()

#define WARNING_SL()                                    \
    s2e()->getWarningsStream()                          \
    << "[" << state->getID() << " / "                   \
    << s2e()->getExecutor()->getStatesCount() << "] "   \
    << " line: " << line << ".  "

#define WARNING_S()                                     \
    s2e()->getWarningsStream()                          \
    << "[" << state->getID() << " / "                   \
    << s2e()->getExecutor()->getStatesCount() << "] "   \

#define WARNING() s2e()->getWarningsStream()


S2E_DEFINE_PLUGIN(SymDriveSearcher, "Prioritizes states that are about to execute unexplored translation blocks",
                  "SymDriveSearcher", "ModuleExecutionDetector");

void SymDriveSearcher::initialize()
{
    // 
    // Module execution
    m_moduleExecutionDetector = static_cast<ModuleExecutionDetector*>(s2e()->getPlugin("ModuleExecutionDetector"));
    m_searcherInited = false;

    m_sorter.p = this;
    m_states = StateSet(m_sorter);

    //XXX: Take care of module load/unload
    m_moduleExecutionDetector->onModuleTranslateBlockEnd.connect(
            sigc::mem_fun(*this, &SymDriveSearcher::onModuleTranslateBlockEnd)
            );
    m_moduleExecutionDetector->onModuleTranslateBlockStart.connect(
            sigc::mem_fun(*this, &SymDriveSearcher::onModuleTranslateBlockStart)
            );

    // Scheduling
    s2e()->getCorePlugin()->onCustomInstruction.connect(
        sigc::mem_fun(*this, &SymDriveSearcher::onCustomInstruction));
    m_favorSuccessful = true;
    m_currentTime = 1000;
    m_lastStartTime = 0;
    m_lastState = NULL;

    // Just makes sure we select a state
    assert (m_currentTime - m_lastStartTime > g_time_budget_fs);
    assert (m_currentTime - m_lastStartTime > g_time_budget_mc);

    s2e()->getCorePlugin()->onTimer.connect(
        sigc::mem_fun(*this, &SymDriveSearcher::onTimer));

    // Selection mode
    m_selectionMode = 1;

    // Performance analysis
    m_dumpiomap_enabled = 0;
    m_tracer = (ExecutionTracer*)s2e()->getPlugin("ExecutionTracer");
    assert(m_tracer);
    m_mon = (RawMonitor*)s2e()->getPlugin("RawMonitor");
    assert (m_mon);

    m_tb = NULL;
    m_lastTbTraced = 0;
    m_TrackperfIRQ = TRACKPERF_IRQ_NONE;

    // Symbolic hardware
    m_hw = (SymbolicHardware*)s2e()->getPlugin("SymbolicHardware");
    assert (m_hw);
    s2e()->getCorePlugin()->setIoMapCallback(io_map_callback, this);

    // Basic blocks initialization:
    initializeBB();

    // Miscellaneous initialization
    s2e()->getCorePlugin()->onStateFork.connect(
        sigc::mem_fun(*this, &SymDriveSearcher::onFork));
}

void SymDriveSearcher::initializeBB (void) {
    std::vector<std::string> Sections;
    Sections = s2e()->getConfig()->getListKeys(getConfigKey());

    foreach2(it, Sections.begin(), Sections.end()) {
        MESSAGE() << "SymDriveSearcher - section " << getConfigKey() << "." << *it << '\n';
        std::stringstream sk;
        sk << getConfigKey() << "." << *it;
        initSection(sk.str(), *it);
    }

    int i;
    for (i = 0; i < m_Modules.size(); i++) {
        ReadBBList(m_Modules[i]);
    }
}

void SymDriveSearcher::initSection(const std::string &cfgKey, const std::string &svcId)
{
    bool ok;
    std::string moduleName = s2e()->getConfig()->getString(cfgKey + ".moduleName", "", &ok);
    if (!ok) {
        WARNING() << "You must specify " << cfgKey <<  ".moduleName\n";
        assert(false);
    }

    std::string moduleDir = s2e()->getConfig()->getString(cfgKey + ".moduleDir", "", &ok);
    if (!ok) {
        WARNING() << "You must specify " << cfgKey <<  ".moduleDir\n";
        assert(false);
    }

    SymDriveModuleInfo newMod;
    newMod.m_moduleName = moduleName;
    newMod.m_moduleDir = moduleDir;

    m_Modules.push_back(newMod);
}

void SymDriveSearcher::ReadBBList(SymDriveModuleInfo &module) {
    std::string filename = module.m_moduleDir + "/" + module.m_moduleName + ".bblist";
    FILE *fp = fopen(filename.c_str(), "r");
    if (!fp) {
        WARNING() << "BBLIST: Could not open file " << filename << "\n";
        return;
    }

    MESSAGE() << "BBLIST: Opened file " << filename.c_str() << "\n";

    char buffer[512];
    while (fgets (buffer, sizeof(buffer), fp)) {
        uint64_t start, end;
        char name[512];
        std::string Name;
        char bad_block[512];

        if (buffer[0] != '0' || buffer[1] != 'x') {
            // MESSAGE() << "BBLIST: Skipping line: " << buffer << "\n";
            continue;
        }

        buffer[strlen(buffer) - 1] = 0; // Remove \n

        int args_assigned = sscanf(buffer, "0x%"PRIx64" 0x%"PRIx64" %s %[^\r\t\n]s",
                                   &start, &end, name, bad_block);
        if (args_assigned < 3) {
            std::cerr << "BBLIST: Mcoverage tool - SymDrive failed to parse bblist, line is:" << std::endl;
            std::cerr << buffer << std::endl;
            continue;
        }

        // MESSAGE() << "BBLIST: Read 0x" << hexval(start) << " 0x" << hexval(end) << " " << name << "\n";
        module.m_bytesTotal += end - start + 1;
        if (args_assigned == 4) {
            if (strcmp (bad_block, "MJR_there_is_a_gap") == 0) {
                //std::cerr << "BBLIST: Note that we have a gap at "
                //          << buffer << std::endl;
                module.m_numGaps++;
                module.m_bytesInGaps = end - start + 1;
            } else {
                std::cerr << "BBLIST: error reading bblist file "
                          << buffer << std::endl;
                assert(false);
            }

            // Option 1: Count any code executed in the gap
            // as covering the entire gap.
            // Option 2: Don't count any code executed in the gap
            // as covering any of the gap.
            // Option 2 is more conservative from a coverage perspective.
            // But it does not make sense here.
            // continue;
        }

        unsigned prevCount = module.m_allBbs.size();
        std::pair<s2etools::BasicBlocks::iterator, bool> result;
        result = module.m_allBbs.insert(s2etools::BasicBlock(start, end));
        if (!result.second) {
            MESSAGE() << "BBLIST: Won't insert this block.  Existing block: " << (*result.first).start << "\n";
            continue;
        }

        // Add all instructions to Function
        // We need mappings for all possible instruction addresses in each BB.
        Name = name; // Use some C++ simplifications
        for (int curAddr = start; curAddr <= end; curAddr++) {
            module.m_AddrToFunction[curAddr] = Name;
            if (Name.substr(0, 6) == "prefn_" ||
                Name.substr(0, 7) == "postfn_" ||
                Name.substr(0, 12) == "stubwrapper_") {
                module.m_AddrValid[curAddr] = false;
            } else {
                module.m_AddrValid[curAddr] = true;
            }
        }

        // Function to BBs
        s2etools::BasicBlocks &bbs = module.m_functions[name];
        //XXX: the +1 is here to compensate the broken extraction script, which
        //does not take into account the whole size of the last instruction.
        prevCount = bbs.size();
        bbs.insert(s2etools::BasicBlock(start, end));
        assert(prevCount == bbs.size()-1);

        BBValidate(module);
    }

    BBValidate(module);

    MESSAGE() << "Note that there are " << module.m_numGaps << " gaps.\n";

    if (module.m_allBbs.size() == 0) {
        MESSAGE() << "BBLIST: No BBs found in the list for " << module.m_moduleName
                  << ". Check the format of the file.\n";
    }

    s2etools::Functions::iterator fit;
    unsigned fcnBbCount = 0;
    for (fit = module.m_functions.begin(); fit != module.m_functions.end() ; ++fit) {
        fcnBbCount += (*fit).second.size();
    }
    assert(fcnBbCount == module.m_allBbs.size());
    
    fclose(fp);
}

void SymDriveSearcher::BBValidate (const SymDriveModuleInfo &module) const {
    s2etools::Functions::const_iterator fit;
    unsigned fcnBbCount = 0;
    for (fit = module.m_functions.begin(); fit != module.m_functions.end() ; ++fit) {
        fcnBbCount += (*fit).second.size();
    }

    assert(fcnBbCount == module.m_allBbs.size());
    assert(module.m_AddrToFunction.size() == module.m_AddrValid.size());
}

std::string SymDriveSearcher::AddrToFunction (const ModuleDescriptor *md, uint64_t address) const {
    std::vector<SymDriveModuleInfo>::const_iterator it;
    for (it = m_Modules.begin(); it != m_Modules.end(); ++it) {
        if (md->Name == (*it).m_moduleName) {
            s2etools::BBToFunction::const_iterator cur = (*it).m_AddrToFunction.find(address - md->LoadBase);
            if (cur != (*it).m_AddrToFunction.end()) {
                const std::string candidate = (*cur).second;
                if (candidate != "") {
                    return candidate;
                }
            } else {
                return "";
            }
        }
    }

    return "";
}

std::string SymDriveSearcher::AddrIsValid (const ModuleDescriptor *md, uint64_t address) const {
    std::vector<SymDriveModuleInfo>::const_iterator it;
    for (it = m_Modules.begin(); it != m_Modules.end(); ++it) {
        if (md->Name == (*it).m_moduleName) {
            std::map<uint64_t, bool>::const_iterator cur_isvalid = (*it).m_AddrValid.find(address - md->LoadBase);
            s2etools::BBToFunction::const_iterator cur_fn = (*it).m_AddrToFunction.find(address - md->LoadBase);
            if (cur_isvalid != (*it).m_AddrValid.end()) {
                assert (cur_fn != (*it).m_AddrToFunction.end());
                if ((*cur_isvalid).second) {
                    return (*cur_fn).second;
                }

                // pre/post/stubwrapper
                return "";
            } else {
                // outside module
                return "";
            }
        }
    }
    
    // outside module
    return "";
}

// The idea is to initialize the SymDrive
// searcher only after a module is loaded.
// Before that we just use the default batching
// searcher (S2E default policy)
void SymDriveSearcher::initializeSearcher()
{
    if (m_searcherInited) {
        return;
    }

    s2e()->getExecutor()->setSearcher(this);
    m_searcherInited = true;
}

uint64_t SymDriveSearcher::computeTargetPc(S2EExecutionState *state)
{
    const Instruction *instr = state->pc->inst;

    //Check whether we are the first instruction of the block
    const BasicBlock *BB = instr->getParent();
    if (instr != &*BB->begin()) {
        return 0;
    }

    //There can be only one predecessor jumping to the terminating block (xxx: check this)
    const BasicBlock *PredBB = BB->getSinglePredecessor();
    if (!PredBB) {
        return 0;
    }

    const BranchInst *Bi = dyn_cast<BranchInst>(PredBB->getTerminator());
    if (!Bi) {
        return 0;
    }

    //instr must be a call to tcg_llvm_fork_and_concretize
    MESSAGE() << "SymDriveSearcher: " << *instr << "\n";

    const CallInst *callInst = dyn_cast<CallInst>(instr);
    if (!callInst) {
        return 0;
    }

    assert(callInst->getCalledFunction()->getName() == "tcg_llvm_fork_and_concretize");

    const ConstantInt *Ci = dyn_cast<ConstantInt>(callInst->getOperand(0)); // SymDrive changed from 1 to 0
    if (!Ci) {
        return false;
    }

    const uint64_t* Int = Ci->getValue().getRawData();
    return *Int;
    //return GetPcAssignment(BB);
}

/**
 *  Prioritize the current state while it keeps discovering new blocks.
 *  XXX: the implementation can add the state even if the targetpc is not reachable
 *  because of path constraints.
 */
void SymDriveSearcher::onModuleTranslateBlockEnd(
    ExecutionSignal *signal,
    S2EExecutionState* state,
    const ModuleDescriptor &md,
    TranslationBlock *tb,
    uint64_t endPc,
    bool staticTarget,
    uint64_t targetPc)
{
    initializeSearcher();

    signal->connect(sigc::mem_fun(*this, &SymDriveSearcher::onTraceTbEnd));

    //Done translating the blocks, no need to instrument anymore.
    m_tb = NULL;
    m_tbConnection.disconnect();
}

void SymDriveSearcher::onModuleTranslateBlockStart(
    ExecutionSignal *signal,
    S2EExecutionState *state,
    const ModuleDescriptor &md,
    TranslationBlock *tb,
    uint64_t startPc)
{
    if (m_tb) {
        m_tbConnection.disconnect();
    }
    m_tb = tb;

    // trace each TB
    signal->connect(sigc::mem_fun(*this, &SymDriveSearcher::onTraceTbStart));

    // trace each instruction
    CorePlugin *plg = s2e()->getCorePlugin();
    m_tbConnection = plg->onTranslateInstructionStart.connect(
        sigc::mem_fun(*this, &SymDriveSearcher::onTranslateInstructionStart));
}

// This is called during translation time
void SymDriveSearcher::onTranslateInstructionStart(
    ExecutionSignal *signal,
    S2EExecutionState* state,
    TranslationBlock *tb,
    uint64_t pc) {
    if (tb != m_tb) {
        //We've been suddenly interrupted by some other module
        m_tb = NULL;
        m_tbConnection.disconnect();
        return;
    }

    //connect a function that will increment the number of executed
    //instructions.
    signal->connect(sigc::mem_fun(*this, &SymDriveSearcher::onTraceInstruction));
}

void SymDriveSearcher::onFork(S2EExecutionState *state,
                           const std::vector<S2EExecutionState*> &newStates,
                           const std::vector<klee::ref<klee::Expr> > &newConditions) {
    if (m_favorSuccessful == true) {
        DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
        if (plgState->m_loopStates.size() > 0) {
            unsigned int loop_count = plgState->m_loopStates.back();
            loop_count++;
            MESSAGE_S() << "Forking and tracking loop: " << state->getID()
                        << ", state count: " << loop_count
                        << ", number of entries: " << plgState->m_loopStates.size()
                        << "\n";
            plgState->m_loopStates.pop_back();
            plgState->m_loopStates.push_back(loop_count);
        }
    }

    assert(state->isActive());

    //
    // This code executes regardless of m_favorSuccessful.
    //
    // Delete least priority state if numbers get out of hand
    if (m_states.size() < 100) {
        return;
    }

    int cur_prio;
    StateSet::iterator itAllStates;
    S2EExecutionState *to_remove = NULL;
    if (m_favorSuccessful == true) {
        cur_prio = 0x7FFFFFFF;
    } else {
        cur_prio = 0;
    }

    // Iterate over all states.
    for (itAllStates = m_states.begin(); itAllStates != m_states.end(); itAllStates++) {
        S2EExecutionState *es = dynamic_cast<S2EExecutionState*>(*itAllStates);
        // If the current state we're seeing is the current state
        // we're executing continue and ignore it -- we never kill the
        // current state.
        if (es == state) {
            continue;
        }

        // Make sure we don't kill one of the new states.
        if (newStates.size() == 1) {
            // Simple common case: forking only one other path
            if (es == newStates[0]) {
                continue;
            }
        } else if (newStates.size() > 1) {
            // Complex case where we're forking
            // multiple branches
            bool exists = false;
            std::vector<S2EExecutionState *>::const_iterator itNewStates;
            for (itNewStates = newStates.begin();
                 itNewStates != newStates.end();
                 itNewStates++) {
                S2EExecutionState *newState = *itNewStates;
                if (newState == es) {
                    exists = true;
                    break;
                }
            }

            if (exists) {
                continue;
            }
        } else {
            assert (false && "newStates.size() == 0");
        }

        // Only choose the state for removal if it's not one of the new ones
        // We need this complex test because S2E chokes and dies otherwise.
        DECLARE_PLUGINSTATE(SymDriveSearcherState, es);
        if (m_favorSuccessful == true) {
            if (plgState->m_priorityChange < cur_prio) {
                cur_prio = plgState->m_priorityChange;
                to_remove = es;
            }
        } else {
            // Favor successful = false should work too.
            // TODO make this more comprehensive -- why is this
            // a low priority state?
            if (plgState->m_metricValid &&
                plgState->m_metric > cur_prio) {
                cur_prio = plgState->m_metric;
                to_remove = es;
            }
        }
    }

    if (to_remove != NULL) {
        assert(to_remove->isActive() == false);
        WARNING() << "Destroying state "
                  << to_remove->getID()
                  << ", prio: " << cur_prio << "\n";
        helper_check_invariants(true);
        s2e()->getExecutor()->terminateStateEarly(*to_remove, "Too many states -- killing low priority one");
        helper_check_invariants(true);
    } else {
        WARNING() << "Failed to destroy any state" << "\n";
    }
}

void SymDriveSearcher::onCustomInstruction(S2EExecutionState* state, uint64_t opcode)
{
    uint8_t opc = (opcode>>8) & 0xFF;
    if (opc < 0xB3 || opc > 0xCB) {
        // Outside range of supported opcodes
        return;
    }

    int line = 0;
    bool ok = true;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]),
                                         &line, 4);
    if (!ok) {
        WARNING_S()
            << "ERROR: symbolic argument was passed to SymDriveSearcher "
            " insert_symbolic opcode" << "\n";
    }

    assert(state->isActive () == true);
    helper_check_invariants (true);
    m_states.erase(state);
    helper_check_state_not_exists(state);
    helper_check_invariants (true);

    // We do + 1 on all m_states.size() operations below
    // to compensate for temporary state removal.

    switch (opc) {
        // 0xb1 = make dma memory
        // 0xb2 = free dma memory
        case 0xB3: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_prioritize(state, plgState, line);
            break;
        }
        case 0xB4: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_deprioritize(state, plgState, line);
            break;
        }
        case 0xB5: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_loop_before(state, plgState, line);
            break;
        }
        case 0xB6: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_loop_body(state, plgState, line);
            break;
        }
        case 0xB7: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_loop_after(state, plgState, line);
            break;
        }
        case 0xB8: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_concretize_kill(state, plgState, line);
            break;
        }
        case 0xB9: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_concretize_all(state, plgState, line);
            break;
        }
        case 0xBA: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_kill_all_others(state, plgState, line);
            break;
        }
        case 0xBB: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_driver_call_stack(state, plgState, line);
            break;
        }
        case 0xBC: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_favor_successful(state, plgState, line);
            break;
        }
        // hole
        case 0xBE: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_reset_priorities(state, plgState, line);
            break;
        }
        // hole
        case 0xC0: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_enable_tracing(state, plgState, line);
            break;
        }

        case 0xC1: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_disable_tracing(state, plgState, line);
            break;
        }

        case 0xC2: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_enter_function(state, plgState, line);
            break;
        }

        case 0xC3: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_exit_function(state, plgState, line);
            break;
        }

        case 0xC4: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_is_symbolic_symdrive(state, plgState, line);
            break;
        }

        case 0xC5: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_success_path(state, plgState, line);
            break;
        }

        case 0xC6: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_enter_block(state, plgState, line);
            break;
        }

        case 0xC7: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_primary_fn(state, plgState, line);
            break;
        }

        case 0xC8: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_enable_trackperf(state, plgState, line);
            break;
        }

        case 0xC9: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_disable_trackperf(state, plgState, line);
            break;
        }

        case 0xCA: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_trackperf_fn(state, plgState, line);
            break;
        }

        case 0xCB: {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
            s2e_io_region(state, plgState, line);
            break;
        }

        default:
            assert (false);
    }

    m_states.insert(state);
    helper_check_invariants (true);
}

void SymDriveSearcher::s2e_prioritize (S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    if (m_favorSuccessful == false) {
        assert (plgState->m_priorityChange <= 0);
        assert (plgState->m_loopStates.size() == 0);
        return;
    }

    // s2e_prioritize
    if (plgState->m_priorityChange >= -PRIORITY_FIDDLE) {
        if (plgState->m_priorityChange <= PRIORITY_EXTREME) {
            plgState->m_priorityChange += PRIORITY_FIDDLE;
        } else {
            assert (false && "Really?");
        }
        // Else do nothing since we have a big boost
    } else {
        // Immediately reset it so that a built-up penalty won't hose
        // us forever
        plgState->m_priorityChange = 0;
    }

    // MESSAGE_SL() << "Prioritizing" << "\n";
}

//
// TODO: We really want this function to reschedule immediately
// That is, no further instructions.  See end of function body
// for more information
//
void SymDriveSearcher::s2e_deprioritize(S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    if (m_favorSuccessful == false) {
        assert (plgState->m_priorityChange <= 0);
        assert (plgState->m_loopStates.size() == 0);
        return;
    }

    if (line < 0) {
        line = -line;

        // Subtract until we're not in first place any more.
        StateSet::iterator itAllStates;
        int target_priority = INT_MIN;
        if (m_states.size() > 1) {
            for (itAllStates = m_states.begin(); itAllStates != m_states.end(); itAllStates++) {
                S2EExecutionState *es = dynamic_cast<S2EExecutionState*>(*itAllStates);
                // If the current state we're seeing is the current state
                // we're executing continue and ignore it
                if (es == state) {
                    continue;
                }

                DECLARE_PLUGINSTATE(SymDriveSearcherState, es);
                if (target_priority < plgState->m_priorityChange) {
                    target_priority = plgState->m_priorityChange;
                }
            }

            int delta = plgState->m_priorityChange - target_priority;

            if (delta >= 0) {
                // delta == 0 means we have a tie for first place.
                // Always subtract at least one in that case.
                // Also, subtract one more to be sure this state doesn't
                // end up equal in priority to the next
                delta++;
                plgState->m_priorityChange -= delta;
                MESSAGE_S() << "Deprioritizing by " << delta << " at line " << line << "\n";
            } else {
                // This should not happen -- the currently executing state
                // should be in first place, but this might occur because
                // rescheduling is not instantaneous.
                // If we could force a resechedule immediately we could
                // add an "assert (false);" here.
                // See "throw CpuExitException();"
                WARNING_S() << "Deprioritizing - already not in first place?  Line "
                            << line << "\n";
            }
        } else {
            MESSAGE_S() << "Deprioritizing - not bothering as only one state is available.  Line "
                        << line << "\n";
        }
    } else if (line == 0) {
        // Force it to be awful.
        plgState->m_priorityChange = -PRIORITY_FIDDLE;
        MESSAGE_S() << "Deprioritizing by force: " << -PRIORITY_FIDDLE << "\n";
    } else {
        // This feature reduces the amount of the penalty for a given line over time
        // The idea is that we decrease the amount of penalty with each subsequent
        // penalty, until we hit a minimum, at which point we just don't bother
        // penalizing further since it's evident that it's not doing any good anyway.

        /*
        if (m_currentPenalty.find (line) == m_currentPenalty.end()) {
            m_currentPenalty[line] = MAX_PENALTY;
            plgState->m_priorityChange = -PRIORITY_FIDDLE;
            MESSAGE_SL() << "Deprioritizing by force: " << -PRIORITY_FIDDLE << "\n";
        } else {
            if (m_currentPenalty[line] > 0) {
                if (plgState->m_priorityChange > 0) {
                    plgState->m_priorityChange /= m_currentPenalty[line];
                }
                m_currentPenalty[line] /= 2;
                MESSAGE_SL() << "Deprioritizing by: " << m_currentPenalty[line] << "\n";
            } else {
                MESSAGE_SL() << "Deprioritizing:  letting through failed state.\n";
                // Maybe don't reset?  This kind of issue indicates a problem anyway.
                // m_currentPenalty[line] = MAX_PENALTY; // reset
            }
        }
        */

        /*
          The trouble with that feature is that it lets through failed states
          occasionally.  There is no way to bypass it, so let's revert to
          forcible deprioritization and warn if it gets stuck
        */

        if (m_currentPenalty.find (line) == m_currentPenalty.end()) {
            m_currentPenalty[line] = MAX_PENALTY;
            plgState->m_priorityChange = -PRIORITY_FIDDLE;
            MESSAGE_SL() << "Deprioritizing: " << -PRIORITY_FIDDLE << "\n";
        } else {
            if (m_currentPenalty[line] > 0) {
                m_currentPenalty[line]--;
                MESSAGE_SL() << "Deprioritizing: " << -PRIORITY_FIDDLE << "\n";
            } else {
                WARNING_SL() << "Deprioritizing warning:  repeated deprioritizations, maybe add annotation?\n";
            }

            plgState->m_priorityChange = -PRIORITY_FIDDLE;
        }
    }

    MESSAGE_SL() << "Rescheduling..." << "\n";
    // Add back state
    // Otherwise we'll never reschedule back to it.
    m_states.insert(state);
    helper_check_invariants(true);
    state->writeCpuState(CPU_OFFSET(eip), state->getPc() + 10, 32);
    m_lastState = NULL;

    // Revision 8,204:
    s2e()->getExecutor()->yieldState(*state);

    // Let's try again:
    //state->setRescheduling(true);

    // force reschedule
    //state->writeCpuState(CPU_OFFSET(exception_index), EXCP_S2E, 8*sizeof(int));
    //throw CpuExitException();

    // Don't call directly:
    // s2e()->getExecutor()->stateSwitchTimerCallback(s2e()->getExecutor());
    // qemu_mod_timer(s2e()->getExecutor()->m_stateSwitchTimer, 0); // Fire as fast as possible.

    // We're rescheduling but not immediately :(  TODO add feature
    // to reschedule immediately (no more instructions on current path)
    // ::usleep(100 * 1000); // Maybe this will do the job? Heh
    // See m_stateSwitchTimer and sleep same amt in ms.
}

void SymDriveSearcher::s2e_loop_before(S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    if (m_favorSuccessful == false) {
        assert (plgState->m_priorityChange <= 0);
        assert (plgState->m_loopStates.size() == 0);
        return;
    }

    int call_id = helper_readCallId(state, plgState);

    // Path-specific:
    plgState->m_loopStates.push_back(0);

    // Note that this code applies to all paths:
    if (m_loopCount.find(call_id) == m_loopCount.end()) {
        m_loopCount[call_id] = 0;
    } else {
        // Some path is hitting the same loop.
    }

    MESSAGE_SL() << "Before loop, call_id: " << call_id << "\n";
}

void SymDriveSearcher::s2e_loop_body(S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    if (m_favorSuccessful == false) {
        assert (plgState->m_priorityChange <= 0);
        assert (plgState->m_loopStates.size() == 0);
        return;
    }

    int call_id = helper_readCallId(state, plgState);
    assert (plgState->m_loopStates.size () > 0);

    // Notice that we're executing a loop now.
    // We may increment this repeatedly along different paths.
    m_loopCount[call_id]++;

    // We want to find any state that keeps forking more copies
//    if (m_states.size() + 1 < plgState->m_loopStates.back()) {
//        // In this case, we must have been generating states,
//        // and then we killed a bunch.
//       // Do nothing?  Forking is disabled -- see below.
//        return;
//    }

    if (plgState->m_loopStates.back() == 0) {
        //MESSAGE() << "Loop is not generating states, state: " << state->getID()
        //                        << ", line " << line << "\n";
        // In this case there was no change.
        // Prioritize so we don't lose out.
        s2e_prioritize(state, plgState, line);
        return;
    }

    if (plgState->m_loopStates.back() >= 1 &&
        plgState->m_loopStates.back() <= 20) {
        // In this case, we're generating states but have not generated that many
        MESSAGE_SL() << "Loop generating states, call_id: " << call_id << "\n";

        assert (m_loopCount[call_id] >= 0);

        if (m_loopCount[call_id] == 1) {
            // We've executed one interation of the loop along this path
            // We might fall through at this point, e.g. if it's a polling
            // loop.  Let's do nothing and see.
        } else if (m_loopCount[call_id] == 2) {
            // Deprioritize a lot
            s2e_deprioritize(state, plgState, 0);
        } else if (m_loopCount[call_id] == 3) {
            // Deprioritize only a bit since we've executed
            // this loop three times -- we're really hoping now
            s2e_deprioritize(state, plgState, -line);
        } else {
            // If it's greater, prioritize and hope
            s2e_prioritize(state, plgState, line);
        }
        return;
    }

    if (plgState->m_loopStates.back() > 20) {
        // In this case, each iteration of the loop is growing the number
        // of states.  That's bad.  Deprioritize
        // Actually, just kill it.
        // s2e_deprioritize(state, plgState, line);

        if (plgState->m_loopStates.back() % 10 == 0) {
            WARNING() << "Loop generating many states, state: " << state->getID()
                      << ", call_id: " << call_id
                      << ", line: " << line
                      << ", count: " << plgState->m_loopStates.back()
                      << "\n";
            WARNING() << "Attempting deprioritize...\n";
            s2e_deprioritize(state, plgState, -line);
        } else {
            s2e_prioritize(state, plgState, line);
        }
        return;
    }

    assert(false);
}

void SymDriveSearcher::s2e_loop_after(S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    if (m_favorSuccessful == false) {
        assert (plgState->m_priorityChange <= 0);
        assert (plgState->m_loopStates.size() == 0);
        return;
    }

    int call_id = helper_readCallId(state, plgState);
    if (plgState->m_loopStates.size() == 0) {
        MESSAGE_SL() << "Failure: line " << line << ", call_id = " << call_id << "\n";
        assert (false && "Should have already set loopStates");
    }
    plgState->m_loopStates.pop_back();

    // TODO: Do we need this call:
    // Reasoning about this variable across states is complex
    // because:
    // 1) Nested loops exist
    // 2) Multiple invocations of the same loop exist
    // 3) A path that successfully makes it through a loop may
    //    be deprioritized and end up back in it.

    /*int result = */m_loopCount.erase(call_id);

    // TODO:
    // This assertion is not needed, I think.  The idea is that
    // a path may fork off some copies, then finish the loop, then
    // end up back in the loop when S2E switches
    // assert(result == 1 && "Loop not started?");

    s2e_prioritize(state, plgState, line);
    MESSAGE_SL() << "After loop, call_id: " << call_id << "\n";
}

void SymDriveSearcher::s2e_concretize_kill (S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    if (m_favorSuccessful == false) {
        assert (plgState->m_priorityChange <= 0);
        assert (plgState->m_loopStates.size() == 0);
        return;
    }

    // Concretize everything
    s2e_concretize_all(state, plgState, line);
    // Prioritize ourselves
    s2e_prioritize(state, plgState, line);

    if (plgState->m_driverCallStack > 0) {
        MESSAGE_SL() << "s2e_concretize_kill called, but m_driverCallStack = "
                  << plgState->m_driverCallStack << "\n";
        return;
    }

    // Kill other states
    s2e_kill_all_others (state, plgState, line);
}

void SymDriveSearcher::s2e_concretize_all (S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    StateSet::iterator it;
    MESSAGE_SL() << "Concretizing everything, maybe?" << "\n";
    MESSAGE_SL() << "Switching to concrete execution at pc = "
              << hexval(state->getPc()) << "\n";

    /* Concretize any symbolic registers */
    MESSAGE_SL() << "Concretizing everything (except registers!??  ks8851 workaround)... " << "\n";

    // Concretize everything
    state->addressSpace.concretizeAll(s2e()->getExecutor());
}

void SymDriveSearcher::s2e_kill_all_others (S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    // Only remove other states if we're not being reentrant.
    MESSAGE_SL() << "Removing all other states, count: " << m_states.size() << "\n";

    // Retrieve all states -- this plugin does not normally track a number of other states.
    StateSet::iterator itAllStates;
    for (itAllStates = m_states.begin(); itAllStates != m_states.end(); itAllStates++) {
        S2EExecutionState *es = dynamic_cast<S2EExecutionState*>(*itAllStates);
        if (es != state) {
            s2e()->getExecutor()->terminateStateEarly
                (*es, "Killed because we're removing all states except one");
        } else {
            WARNING() << "Not killing current state: " << es->getID() << "\n";
        }
    }

    // We re-add state after end of opcode switch statement.
    m_states.clear();
    helper_check_invariants(true);
}

void SymDriveSearcher::s2e_driver_call_stack (S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    int driver_call_stack = 0;
    bool ok = true;

    // Execute this function regardless of m_favorSuccessful
    // since this way the value will be all set if we switch back

    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                         &driver_call_stack, 4);
    if (!ok) {
        WARNING_S() << "ERROR: symbolic argument was passed to " << __func__ << "\n";
    }

    plgState->m_driverCallStack = driver_call_stack;
}

void SymDriveSearcher::s2e_favor_successful (S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    int successful = 0;
    bool ok = true;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                         &successful, 4);
    if (!ok) {
        WARNING_S() << "ERROR: symbolic argument was passed to " << __func__ << "\n";
    }

    if (successful == 0) {
        if (m_favorSuccessful == true) {
            m_favorSuccessful = false;
            WARNING_S() << "s2e_favor_successful: false" << "\n";

            // Now, we're no longer favoring successful states, so we need to back out
            // all the priority hacking crap we've been doing.
            s2e_reset_priorities(state, plgState, line);

            // ethtool functions don't really need this
            // but sometimes we do.
            // s2e_kill_all_others(state, plgState, line);
        }
        // else do nothing -- this is a no-op.
    } else {
        // This has really weird semantics and it's not clear when we'd ever
        // want to set it to false and then subsequently re-enable it along
        // this branch.
        //
        // TODO: Can we do anything interesting along this branch?
        //
        WARNING_S() << "s2e_favor_successful: true" << "\n";
        m_favorSuccessful = true;
    }
}

void SymDriveSearcher::s2e_reset_priorities(S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    // Reset all statistics
    WARNING_S() << "s2e_reset_priorities:  resetting priorities to 0" << "\n";
    StateSet::iterator itAllStates;

    helper_check_invariants(true);
    m_states.insert(state);
    helper_check_invariants(true);
    std::set<klee::ExecutionState*> states_copy;
    for (itAllStates = m_states.begin(); itAllStates != m_states.end(); itAllStates++) {
        S2EExecutionState *es = dynamic_cast<S2EExecutionState*>(*itAllStates);
        DECLARE_PLUGINSTATE(SymDriveSearcherState, es);
        plgState->m_priorityChange = 0;
        plgState->m_loopStates.clear();

        states_copy.insert(es);
    }

    // We need to reset our set
    WARNING_S() << "s2e_favor_successful:  re-adding all states..." << "\n";

    // Re-set all states:
    std::set<klee::ExecutionState*> empty_set;
    m_states.clear();
    helper_check_invariants(true);
    update (state, states_copy, empty_set);
}

void SymDriveSearcher::s2e_enable_tracing(S2EExecutionState *state, SymDriveSearcherState* plgState, int line)
{
    MESSAGE_SL() << "Enabling I/O memory tracing" << "\n";
    g_tracing_enabled = 1;
    m_memoryMonitor.disconnect();
    m_memoryMonitor = s2e()->getCorePlugin()->onIOMemoryAccess.connect(
        sigc::mem_fun(*this, &SymDriveSearcher::onIOMemoryAccess));
}

void SymDriveSearcher::s2e_disable_tracing(S2EExecutionState *state,
                                        SymDriveSearcherState* plgState,
                                        int line)
{
    MESSAGE_SL() << "Disabling I/O memory tracing" << "\n";
    m_memoryMonitor.disconnect();
    g_tracing_enabled = 0;
}

void SymDriveSearcher::s2e_enterexit_function(S2EExecutionState *state,
                                           SymDriveSearcherState* plgState,
                                           int line,
                                           bool enter) {
    uint32_t fn_address; // 32-bit only
    bool ok = state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                             &fn_address, 4);
    if(!ok) {
        WARNING_S() << "ERROR: symbolic argument was passed to s2e_enter_function 1." << "\n";
        return;
    }

    std::string fn = "";
    if(!state->readString(fn_address, fn)) {
        WARNING_S() << "Error reading s2e_enter_function param" << "\n";
        return;
    }

//    enum symdrive_WRAPPER_TYPE // Must match test framework definition
//    {
//        STUBWRAPPER = 0,
//        PREPOSTFN = 1
//    };

    int wrapper_type;
    ok = state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                        &wrapper_type, 4);
    if(!ok) {
        WARNING_S() << "ERROR: symbolic argument was passed to s2e_enter_function 2." << "\n";
        return;
    }

    if (wrapper_type == 0) { // stubwrapper
        if (enter == false) {
            helper_push_driver_call_stack(state, plgState, fn, line);
        } else {
            helper_pop_driver_call_stack(state, plgState, fn, line);
        }

        m_FunctionCounts[fn]++; // Keep track of global function counts
    } else { // driver function
        if (enter == true) {
            helper_push_driver_call_stack(state, plgState, fn, line);
        } else {
            helper_pop_driver_call_stack(state, plgState, fn, line);
        }
    }
}

void SymDriveSearcher::s2e_enter_function(S2EExecutionState *state,
                                       SymDriveSearcherState* plgState,
                                       int line)
{
    s2e_enterexit_function(state, plgState, line, true);
}

void SymDriveSearcher::s2e_exit_function(S2EExecutionState *state,
                                      SymDriveSearcherState* plgState,
                                      int line)
{
    s2e_enterexit_function(state, plgState, line, false);
}

void SymDriveSearcher::s2e_is_symbolic_symdrive(S2EExecutionState *state,
                                                SymDriveSearcherState* plgState,
                                                int line)
{
    ref<Expr> expr = state->readCpuRegister(CPU_OFFSET(regs[R_EBX]),
                                            4 * 8); // 4 bytes = good enough
    uint32_t v = 0;
    if(!isa<klee::ConstantExpr>(expr)) {
        // false = not concrete
        v = 1;
    } else {
        v = 0;
    }

    state->writeCpuRegisterConcrete(CPU_OFFSET(regs[R_EAX]), &v, 4);
}

void SymDriveSearcher::s2e_success_path(S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    uint32_t fn_address; // 32-bit only
    int success_path = 0;
    bool ok;

    // This code executes regardless of m_favorSuccessful
    ok = state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                        &fn_address, 4);
    if(!ok) {
        WARNING_S() << "ERROR: symbolic argument was passed to s2e_success_path 1." << "\n";
        return;
    }

    ok = state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                        &success_path, 4);

    assert (success_path >= -1 && success_path <= 1);

    std::string fn = "";
    if(!state->readString(fn_address, fn)) {
        WARNING_S() << "Error reading s2e_success_path param 2" << "\n";
        return;
    }

    if (success_path == 1) {
        if (plgState->m_successPath >= 0) {
            plgState->m_successPath += success_path;
        } else {
            // Once failed - always failed
            plgState->m_successPath -= success_path;
        }
    } else if (success_path == -1) {
        plgState->m_successPath += success_path;
        // Once failed - always failed.
    } else if (success_path == 0) {
        plgState->m_successPath = 0;
    } else {
        assert (false && "Add or subtract 1 for success_path, or reset with 0");
    }

    helper_ETraceSuccess(state, fn, plgState->m_successPath);
}

void SymDriveSearcher::s2e_enter_block(S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    bool ok = true;
    uint32_t str_address;
    int total_blocks;
    int cur_block;

    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                         &str_address, 4);
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                         &total_blocks, 4);
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EDX]),
                                         &cur_block, 4);
    if (!ok) {
        WARNING() << "Check s2e_enter_block call: "
                                   << total_blocks
                                   << ", cur: "
                                   << cur_block
                                   << "\n";
        assert(false);
    }

    std::string function = "";
    if(!state->readString(str_address, function)) {
        WARNING_S() << "Error reading s2e_enter_block param" << "\n";
        return;
    }

    // DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
    m_BlockCounts[function].function = function;
    m_BlockCounts[function].total_blocks = total_blocks;
    m_BlockCounts[function].blocks_touched[cur_block] = true;

/*
    MESSAGE_SL() << "Function: " << function << ", "
              << "Total blocks: " << total_blocks << ", "
              << "Current block: " << cur_block
              << "\n";
*/
}

void SymDriveSearcher::s2e_primary_fn(S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    bool ok = true;
    uint32_t str_address;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                         &str_address, 4);
    std::string function = "";
    if(!state->readString(str_address, function)) {
        WARNING_S() << "Error reading s2e_primary_fn param" << "\n";
        return;
    }

    if (function.size() == 0) {
        MESSAGE_SL() << "Not adding primary function because it's empty" << "\n";
        return;
    }

    MESSAGE_SL() << "Adding primary function: " << function << "\n";
    m_PrimaryFn.push_back(function);
}

void SymDriveSearcher::s2e_enable_trackperf(S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    int op;
    bool ok = state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]), &op, 4);
    if (!ok) {
        WARNING()
            << "Check s2e_enable_trackperf line "
            << line << "\n";
        assert(false);
    }

    std::string push_pop = "ERROR";
    switch (op) {
        case START_AUTO:
        case START_MANUAL:
            push_pop = "PUSH";
            break;

        case CONTINUE_PP:
        case CONTINUE_STUB:
        case CONTINUE_IRQ:
        case CONTINUE_AUTO:
        case CONTINUE_MANUAL:
            push_pop = "POP";
            break;

        // Special cases:  These are more like runtime parameters
        case TRACKPERF_IRQ_ALL:
            MESSAGE_SL() << "Trackperf:  TRACKPERF_IRQ_ALL\n";
            m_TrackperfIRQ = TRACKPERF_IRQ_ALL;
            return;
        case TRACKPERF_IRQ_ONLY_CALLED:
            MESSAGE_SL() << "Trackperf:  TRACKPERF_IRQ_ONLY_CALLED\n";
            m_TrackperfIRQ = TRACKPERF_IRQ_ONLY_CALLED;
            return;
        case TRACKPERF_IRQ_NONE:
            MESSAGE_SL() << "Trackperf:  TRACKPERF_IRQ_NONE\n";
            m_TrackperfIRQ = TRACKPERF_IRQ_NONE;
            return;

        // Anything else is nonsense in this context:
        default:
            MESSAGE_SL() <<" Trackperf failure: " << op << "\n";
            assert (false);
    }

    if (push_pop == "PUSH") {
        assert (plgState->m_TrackPerf.size() <= 100);
        plgState->m_TrackPerf.push_back(op);
    } else if (push_pop == "POP") {
        int size = plgState->m_TrackPerf.size();
        if (size < 1) {
            MESSAGE_SL() << "Trying to pop nonexistent state.  Line: " << line << ", op: " << op << "\n";
            assert (false);
        }

        int correspondence = 0;
        if (op == CONTINUE_PP) correspondence = PAUSE_PP;
        if (op == CONTINUE_STUB) correspondence = PAUSE_STUB;
        if (op == CONTINUE_IRQ) correspondence = PAUSE_IRQ;
        if (op == CONTINUE_AUTO) correspondence = PAUSE_AUTO;
        if (op == CONTINUE_MANUAL) correspondence = PAUSE_MANUAL;
        assert (correspondence);

        int i;
        bool removed = false;
        for (i = size - 1;
             i >= 0;
             i--) {
            if (plgState->m_TrackPerf[i] == correspondence) {
                plgState->m_TrackPerf.erase(plgState->m_TrackPerf.begin() + i);
                removed = true;
                break;
            }
        }

        assert (removed);
    } else {
        assert (false);
    }

    std::string list = "";
    list += helper_dump_trackperf(state, plgState->m_TrackPerf);
    helper_ETraceEvent(state, op);
    // MESSAGE_SL() << "Enabling performance tracking (" << op << ").  " << list << "\n";
}

void SymDriveSearcher::s2e_disable_trackperf(S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    int op;
    bool ok = state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]), &op, 4);
    if (!ok) {
        WARNING() << "Check s2e_disable_trackperf line "
                                   << line
                                   << "\n";
        assert(false);
    }

    std::string push_pop = "ERROR";
    switch (op) {
        case (PAUSE_PP):
        case (PAUSE_STUB):
        case (PAUSE_IRQ):
        case (PAUSE_AUTO):
        case (PAUSE_MANUAL):
            push_pop = "PUSH";
            break;

        case (STOP_AUTO):
        case (STOP_MANUAL):
        case (DISCARD_AUTO):
        case (DISCARD_MANUAL):
            push_pop = "POP";
            break;

        default:
            assert (false);
    }

    if (push_pop == "PUSH") {
        // Disable tracking for now.
        plgState->m_TrackPerf.push_back(op);
    } else if (push_pop == "POP") {
        int size = plgState->m_TrackPerf.size();
        assert (size >= 1);

        int correspondence = 0;
        if (op == STOP_AUTO) correspondence = START_AUTO;
        if (op == DISCARD_AUTO) correspondence = START_AUTO;
        if (op == STOP_MANUAL) correspondence = START_MANUAL;
        if (op == DISCARD_MANUAL) correspondence = START_MANUAL;
        assert (correspondence);

        int i;
        bool removed = false;
        for (i = size - 1;
             i >= 0;
             i--) {
            if (plgState->m_TrackPerf[i] == correspondence) {
                plgState->m_TrackPerf.erase(plgState->m_TrackPerf.begin() + i);
                removed = true;
                break;
            }
        }

        assert (removed);
    } else if (push_pop == "CLEAR") {
        plgState->m_TrackPerf.clear();
    } else {
        assert (false);
    }

    //MESSAGE_SL() << "Disabling performance tracking (" << op << ").  "
    //             << helper_dump_trackperf(state, plgState->m_TrackPerf) << "\n";

    if (op == STOP_AUTO || op == STOP_MANUAL) {
        helper_perf_store(state, plgState);
        MESSAGE() << helper_dump_allperf(state);
    }

    if (op == STOP_AUTO || op == STOP_MANUAL || op == DISCARD_AUTO || op == DISCARD_MANUAL) {
        helper_perf_reset(state, plgState);
    }

    helper_ETraceEvent(state, op);
}

void SymDriveSearcher::s2e_trackperf_fn(S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    bool ok = true;
    uint32_t str_address;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                         &str_address, 4);
    std::string function = "";
    if(!state->readString(str_address, function)) {
        WARNING_S() << "Error reading s2e_trackperf_fn param" << "\n";
        return;
    }

    if (function.size() == 0) {
        MESSAGE_SL() << "Not adding trackperf function because it's empty" << "\n";
        return;
    }

    uint32_t flags;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                         &flags, 4);
    
    MESSAGE_SL() << "Adding trackperf function: " << function << ", flags " << flags << "\n";
    m_TrackperfFn.push_back(function);
    m_TrackperfFnFlags.push_back(flags);
    assert (flags == TRACKPERF_NONTRANSITIVE ||
            flags == TRACKPERF_TRANSITIVE ||
            flags == TRACKPERF_IRQ_HANDLER);
    assert (m_TrackperfFn.size() == m_TrackperfFnFlags.size());
}

void SymDriveSearcher::s2e_io_region(S2EExecutionState *state, SymDriveSearcherState* plgState, int line) {
    bool ok = true;
    uint32_t flags;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                         &flags, 4);
    uint32_t address;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_ECX]),
                                         &address, 4);
    uint32_t size;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EDX]),
                                         &size, 4);

    assert (flags == s2e::plugins::IO_MAP ||
            flags == s2e::plugins::IO_UNMAP ||
            flags == s2e::plugins::PORT_MAP ||
            flags == s2e::plugins::PORT_UNMAP);
    assert (ok && "Make sure s2e_io_region is used properly.  Who needs documentation when you have assert?");

    MESSAGE_SL() << "s2e_io_region. line: " << line <<
        ", flags: " << flags <<
        ", address: " << hexval(address) <<
        ", size: " << hexval(size) <<
        "\n";

    // Using S2E trace infrastructure too
    ExecutionTraceIORegion e;
    memset(&e, 0, sizeof(e));
    e.state_id = state->getID();
    e.pc = state->getPc();
    e.flags = (enum s2e::plugins::TRACE_IO_REGION) flags;
    e.address = address;
    e.size = size;
    m_tracer->writeData(state, &e, sizeof(e), TRACE_IO_REGION);
}

#ifdef ENABLE_INVARIANTS
void SymDriveSearcher::helper_check_invariants(bool full_checks) {
    // Check for duplicates
    std::map<S2EExecutionState*, unsigned> counter1;
    std::map<unsigned, unsigned> counter2;
    std::map<SymDriveSearcherState*, unsigned> counter3;
    foreach2(it1, m_states.begin(), m_states.end()) {
        DECLARE_PLUGINSTATE_N(SymDriveSearcherState, it1_state, *it1);
        ++counter1[*it1];
        ++counter2[(*it1)->getID()];
        ++counter3[it1_state];
    }

    foreach2(it1, counter1.begin(), counter1.end()) {
        if (it1->second != 1) {
            WARNING() << "State: " << it1->first->getID() << "\n";
            assert (false && "State found multiple times (error 1).\n");
        }
    }

    foreach2(it1, counter2.begin(), counter2.end()) {
        if (it1->second != 1) {
            WARNING() << "State: " << it1->first << "\n";
            assert (false && "State found multiple times (error 2).\n");
        }
    }

    foreach2(it1, counter3.begin(), counter3.end()) {
        if (it1->second != 1) {
            assert (false && "State found multiple times (error 3).\n");
        }
    }

    // Check ordering
    S2EExecutionState *es2 = NULL;
    SymDriveSorter sorter;
    sorter.p = this;
    foreach2(it1, m_states.begin(), m_states.end()) {
        S2EExecutionState *es1 = *it1;
        if (es2 != NULL) {
            assert (es2 != es1);
            DECLARE_PLUGINSTATE_N(SymDriveSearcherState, es1_state, es1);
            DECLARE_PLUGINSTATE_N(SymDriveSearcherState, es2_state, es2);
            if (sorter (es2, es1) == false) {
                WARNING()
                    << "es1_state: " << hexval((unsigned long) es1_state)
                    << ", priorityChange: " << es1_state->m_priorityChange
                    << ", m_metric: " << es1_state->m_metric
                    << ", m_metricValid: " << (es1_state->m_metricValid ? "true" : "false")
                    << ", id: " << es1->getID()
                    << "\n";
                WARNING()
                    << "es2_state: " << hexval((unsigned long) es2_state)
                    << ", priorityChange: " << es2_state->m_priorityChange
                    << ", m_metric: " << es2_state->m_metric
                    << ", m_metricValid: " << (es2_state->m_metricValid ? "true" : "false")
                    << ", id: " << es2->getID()
                    << "\n";
                assert (false);
            }
        }
        es2 = *it1;
    }

    // Check that we can find everything
    foreach2(it1, m_states.begin(), m_states.end()) {
        S2EExecutionState *es = *it1;
        if (m_states.find(es) == m_states.end()) {
            assert(0 && "Unable to find state that's present");
        }
    }

    //assert (m_states.size() <= m_states.size() + 1);
}

void SymDriveSearcher::helper_check_state_not_exists(S2EExecutionState *state) const {
    assert(m_states.find (state) == m_states.end () && "primary m_states.find failure");

    bool found = false;
    foreach2(it1, m_states.begin(), m_states.end()) {
        if ((*it1) == state) {
            found = true;
            break;
        }
    }

    assert (!found && "secondary m_states.find failure -- this indicates improper set usage");
}
#else // ENABLE_INVARIANTS
void SymDriveSearcher::helper_check_invariants(bool full_checks) {
}

void SymDriveSearcher::helper_check_state_not_exists(S2EExecutionState *state) const {
}
#endif

void SymDriveSearcher::helper_dump_priorities(S2EExecutionState *state) const {
    DECLARE_PLUGINSTATE(SymDriveSearcherState, state);

    MESSAGE() << "==================================================" << "\n";
    std::string stack = helper_driver_call_stack(plgState);
    MESSAGE() << stack << "\n";
    foreach2(it1, m_states.begin(), m_states.end()) {
        S2EExecutionState *state = *it1;
        DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
        MESSAGE()
            << "[State " << state->getID() << "]"
            << " pChg:" << plgState->m_priorityChange
            << " met:" << plgState->m_metric
            << "" << (plgState->m_metricValid ? "t" : "f")
            << " sPth:" << plgState->m_successPath
            << " cDep:" << plgState->m_driverCallStack
            << " IP:" << hexval(state->getPc())
            << " BB:"  << number_to_string(plgState->m_curTrackPerf[SymDriveSearcherState::BB])
            << " I:"  << number_to_string(plgState->m_curTrackPerf[SymDriveSearcherState::INST])
            << " PR:" << number_to_string(plgState->m_curTrackPerf[SymDriveSearcherState::PIO_Read])
            << " PW:" << number_to_string(plgState->m_curTrackPerf[SymDriveSearcherState::PIO_Write])
            << " MR:" << number_to_string(plgState->m_curTrackPerf[SymDriveSearcherState::MMIO_Read])
            << " MW:" << number_to_string(plgState->m_curTrackPerf[SymDriveSearcherState::MMIO_Write])
            << " DR:" << number_to_string(plgState->m_curTrackPerf[SymDriveSearcherState::DMA_Read])
            << " DW:" << number_to_string(plgState->m_curTrackPerf[SymDriveSearcherState::DMA_Write])
            << "\n";
    }
    MESSAGE() << "==================================================" << "\n";
}

bool SymDriveSearcher::helper_pointless_function(std::string fn) const {
    static std::string pointless_functions[] = {
        "inb",
        "inb_local",
        "inw",
        "inw_local",
        "inl",
        "inl_local",
        "outb",
        "outb_local",
        "outw",
        "outw_local",
        "outl",
        "outl_local",
        "writeb",
        "writew",
        "writel",
        "readb",
        "readw",
        "readl",
        ""
    };

    int j;
    bool pointless = false;
    for (j = 0;; j++) {
        if (pointless_functions[j] == "") {
            break;
        }

        if (fn == pointless_functions[j]) {
            pointless = true;
            break;
        }
    }

    return pointless;
}

std::string SymDriveSearcher::helper_driver_call_stack(const SymDriveSearcherState *plgState) const {
    std::string str_function;
    if(plgState->m_functionCallStackFn.empty()) {
        str_function = "Not in driver";
    } else {
        int i;
        char temp[128];
        for (i = 0; i < (int) plgState->m_functionCallStackFn.size(); i++) {
            sprintf (temp, "%s:%d -> ",
                     plgState->m_functionCallStackFn[i].c_str(),
                     plgState->m_functionCallStackLine[i]);
            str_function += temp;
        }
    }

    return str_function;
}

void SymDriveSearcher::helper_ETraceInstr (S2EExecutionState *state, uint64_t pc, uint64_t delta, std::string fn) const {
    // Using S2E trace infrastructure too
    ExecutionTraceInstr e;
    memset(&e, 0, sizeof(e));
    e.state_id = state->getID();
    e.pc = pc;
    e.delta = delta;
    strncpy (e.fn, fn.c_str(), 32);
    m_tracer->writeData(state, &e, sizeof(e), TRACE_INSTR);
}

void SymDriveSearcher::helper_ETraceBB (S2EExecutionState *state, uint64_t pc, uint64_t delta, std::string fn) const {
    ExecutionTraceBB e;
    memset(&e, 0, sizeof(e));
    e.state_id = state->getID();
    e.pc = pc;
    e.delta = delta;
    strncpy (e.fn, fn.c_str(), 32);
    m_tracer->writeData(state, &e, sizeof(e), TRACE_BB);
}

void SymDriveSearcher::helper_ETraceEvent(S2EExecutionState *state, int event) const {
    ExecutionTraceEvent e;
    memset(&e, 0, sizeof(e));
    e.state_id = state->getID();
    e.pc = state->getPc();
    e.event = event;
    m_tracer->writeData(state, &e, sizeof(e), TRACE_EVENT);
}

void SymDriveSearcher::helper_ETraceSuccess (S2EExecutionState *state, std::string fn, uint64_t success) const {
    ExecutionTraceSuccessPath e;
    e.state_id = state->getID();
    e.pc = state->getPc();
    strncpy (e.fn, fn.c_str(), 32);
    e.success = success;
    m_tracer->writeData(state, &e, sizeof(e), TRACE_SUCCESS);
}

void SymDriveSearcher::helper_dump_io_map (S2EExecutionState *state) const {
    int counter = 1;
    DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
    foreach2(it, plgState->m_ioMap.begin(), plgState->m_ioMap.end()) {
        MESSAGE_S() << counter << ", tag: " << (*it).first << " --> " << (*it).second << "\n";
        counter++;
    }
}

std::string SymDriveSearcher::helper_dump_allperf (S2EExecutionState *state) const {
    DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
    StateSet::iterator itAllStates;
    std::string list = "";

    // Print current state information:
    SymDriveSearcherState::RECORDED_OPERATIONS op;
    std::string name;
    for (op = SymDriveSearcherState::BB; op < SymDriveSearcherState::TRACKPERF_COUNT; op++) {
        switch(op) {
            case SymDriveSearcherState::BB: name = "BB"; break;
            case SymDriveSearcherState::INST: name = "INST"; break;
            case SymDriveSearcherState::PIO_Read: name = "PIO_Read"; break;
            case SymDriveSearcherState::PIO_Write: name = "PIO_Write"; break;
            case SymDriveSearcherState::MMIO_Read: name = "MMIO_Read"; break;
            case SymDriveSearcherState::MMIO_Write: name = "MMIO_Write"; break;
            case SymDriveSearcherState::DMA_Read: name = "DMA_Read"; break;
            case SymDriveSearcherState::DMA_Write: name = "DMA_Write"; break;
            default: assert (0);
        }

        list += helper_dump_perf(state, plgState->m_prevTrackPerf[op], name);

        // Print remaining states:
        for (itAllStates = m_states.begin(); itAllStates != m_states.end(); itAllStates++) {
            S2EExecutionState *tempState = dynamic_cast<S2EExecutionState*>(*itAllStates);
            DECLARE_PLUGINSTATE_N(SymDriveSearcherState, tempPlgState, tempState);
            list += helper_dump_perf(tempState, tempPlgState->m_prevTrackPerf[op], name);
        }
    }

    return list;
}


std::string SymDriveSearcher::helper_dump_perf (S2EExecutionState *state,
                                             std::vector<int> &array,
                                             std::string name) const {
    std::vector<int>::iterator it;
    std::string list;
    int count;

    if (array.size() == 0) {
        return "";
    }

    list = "State ";
    list += number_to_string (state->getID());
    list += " ";
    list += name;
    list += " ";

    for (it = array.begin(), count = 0;
         it != array.end();
         it++, count++) {
        list += number_to_string(*it);
        if (count < array.size() - 1) {
            list += ", ";
        }
    }

    list += "\n";
    return list;
}

std::string SymDriveSearcher::helper_dump_trackperf (S2EExecutionState *state,
                                                  std::vector<int> &array) const {
    std::vector<int>::iterator it;
    std::string list;
    int count;
    list = "State ";
    list += number_to_string (state->getID());
    list += " ";

    for (it = array.begin(), count = 0;
         it != array.end();
         it++, count++) {
        std::string str = "ERROR";
        switch (*it) {
            case PAUSE_PP: str = "PAUSE_PP"; break;
            case CONTINUE_PP: str = "CONTINUE_PP"; break;

            case PAUSE_STUB: str = "PAUSE_STUB"; break;
            case CONTINUE_STUB: str = "CONTINUE_STUB"; break;

            case PAUSE_IRQ: str = "PAUSE_IRQ"; break;
            case CONTINUE_IRQ: str = "CONTINUE_IRQ"; break;

            case PAUSE_AUTO: str = "PAUSE_AUTO"; break;
            case START_AUTO: str = "START_AUTO"; break;
            case CONTINUE_AUTO: str = "CONTINUE_AUTO"; break;
            case STOP_AUTO: str = "STOP_AUTO"; break;
            case DISCARD_AUTO: str = "DISCARD_AUTO"; break;

            case START_MANUAL: str = "START_MANUAL"; break;
            case PAUSE_MANUAL: str = "PAUSE_MANUAL"; break;
            case CONTINUE_MANUAL: str = "CONTINUE_MANUAL"; break;
            case STOP_MANUAL: str = "STOP_MANUAL"; break;
            case DISCARD_MANUAL: str = "DISCARD_MANUAL"; break;

            default: assert (false);
        }

        list += str;
        if (count < array.size() - 1) {
            list += ", ";
        }
    }
    return list;
}

bool SymDriveSearcher::helper_should_trackperf(S2EExecutionState *state,
                                            uint64_t pc,
                                            const ModuleDescriptor **curModule,
                                            const ModuleDescriptor **md,
                                            std::string *function) const {
    DECLARE_PLUGINSTATE_CONST(SymDriveSearcherState, state);

    int size = plgState->m_TrackPerf.size();
    if (size == 0) {
        return false;
    }

    int back1 = -1, back2 = -1;
    if (size >= 1) {
        back1 = plgState->m_TrackPerf[size - 1];
    }
    if (size >= 2) {
        back2 = plgState->m_TrackPerf[size - 2];
    }

    bool condition1 = back1 == START_MANUAL;
    //bool condition2 = back1 == START_AUTO &&
    //    back2 != PAUSE_PP &&
    //    back2 != PAUSE_STUB &&
    //    back2 != PAUSE_IRQ;
    bool condition2 = false;
    bool condition3 = plgState->m_TrackperfFnCnt > 0;

    // If someone put in an annotation, then start regardless.
    if (condition1 || condition2 || condition3) {
        if (pc != 0 && curModule != NULL && md != NULL && function != NULL) {
            *curModule = m_moduleExecutionDetector->getModule(state, pc);
            if (!(*curModule)) {
                return false;
            }

            *md = m_moduleExecutionDetector->getCurrentDescriptor(state);
            if (!(*md)) {
                return false;
            }

            if ((*md)->PrimaryModule) {
                // uint64_t pc = state->getTb()->pc;
                // true if not in pre/post/stubwrapper, false otherwise.
                *function = AddrIsValid(*md, pc);
                if (*function != "") {
                    return true;
                } else {
                    return false;
                }
            }

            // Not in primary module, e.g. test_framework
            return false;
        }

        // Tracking enabled but no PC specified so just do it.
        return true;
    } else {
        return false;
    }
}

void SymDriveSearcher::helper_perf_store(S2EExecutionState *state, SymDriveSearcherState *plgState) {
    for (int i = 0; i < SymDriveSearcherState::TRACKPERF_COUNT; i++) {
        plgState->m_prevTrackPerf[i].push_back(plgState->m_curTrackPerf[i]);
    }
}

void SymDriveSearcher::helper_perf_reset(S2EExecutionState *state, SymDriveSearcherState *plgState) {
    // Reset counters
    for (int i = 0; i < SymDriveSearcherState::TRACKPERF_COUNT; i++) {
        plgState->m_curTrackPerf[i] = 0;
    }
}

int SymDriveSearcher::helper_readCallId (S2EExecutionState *state, SymDriveSearcherState *plgState) {
    int call_id = 0;
    bool ok = true;
    ok &= state->readCpuRegisterConcrete(CPU_OFFSET(regs[R_EBX]),
                                         &call_id, 4);
    if (!ok) {
        WARNING_S() << "ERROR: symbolic argument was passed to SymDriveSearcher "
                       " insert_symbolic opcode" << "\n";
    }
    assert (call_id != 0 && "call_id should be nonzero");
    return call_id;
}

void SymDriveSearcher::helper_push_driver_call_stack(S2EExecutionState *state,
                                                  SymDriveSearcherState *plgState,
                                                  std::string fn, int line) {
    plgState->m_functionCallStackFn.push_back(fn);
    plgState->m_functionCallStackLine.push_back(line);
    helper_push_pop_trackperf(state, plgState, START_FN);

}

void SymDriveSearcher::helper_pop_driver_call_stack(S2EExecutionState *state,
                                                 SymDriveSearcherState *plgState,
                                                 std::string fn, int line) {
    // std::string last = plgState->m_functionCallStackFn.back();
    // std::string stack = helper_driver_call_stack(plgState);

    // pop
    int i;
    assert(plgState->m_functionCallStackFn.size() ==
           plgState->m_functionCallStackLine.size());
    for (i = plgState->m_functionCallStackFn.size() - 1; i >= 0; i--) {
        if (plgState->m_functionCallStackFn[i] == fn) {
            plgState->m_functionCallStackFn.erase(plgState->m_functionCallStackFn.begin() + i);
            plgState->m_functionCallStackLine.erase(plgState->m_functionCallStackLine.begin() + i);
            break;
        }
    }

    helper_push_pop_trackperf(state, plgState, STOP_FN);
}

void SymDriveSearcher::helper_push_pop_trackperf(S2EExecutionState *state,
                                              SymDriveSearcherState *plgState,
                                              int event) {
    // This all could be made more efficient but we're only talk about a small number
    // of string comparisons on each function entry/exit.
    int i, j;
    int stack_size = plgState->m_functionCallStackFn.size();
    
    // Reset everything
    plgState->m_TrackperfFnCnt = 0;

    // Things to keep in mind:
    // Interrupt handlers are called by SymDrive so measuring
    //  them too is almost inevitably a bad idea.
    // Interrupt handlers call other functions that we may
    //  want to track, so in that case, we should track the
    //  function of interest.
    int irq_depth = 0;
    for (j = 0; j < plgState->m_functionCallStackFn.size(); j++) {
        std::string cur_fn = plgState->m_functionCallStackFn[j];
        if (irq_depth == 0) {
            for (i = 0; i < m_TrackperfFn.size(); i++) {
                // This is irritating
                if (m_TrackperfFnFlags[i] == TRACKPERF_IRQ_HANDLER &&
                    m_TrackperfFn[i] == cur_fn) {
                    irq_depth++;
                    break;
                }
            }
        }
        else {
            irq_depth++;
        }
        
        // irq_depth now contains the depth of the current
        // function below the IRQ handler, where 1 =
        // the interrupt handler itself, 2 = an fn called
        // by the IRQ handler, 3 = an fn called by an fn
        // called by the IRQ handler, etc.

        if (irq_depth == 0) {
        } else if (irq_depth == 1) {
            // This means we do no tracking as soon as we
            // see an interrupt handler on the stack.
            if (m_TrackperfIRQ == TRACKPERF_IRQ_NONE) {
                plgState->m_TrackperfFnCnt = 0;
                return;
            }
            // We may have tracked_fn --> irq_handler
            // or irq_handler --> tracked_fn
            else if (m_TrackperfIRQ == TRACKPERF_IRQ_ONLY_CALLED) {
                if (plgState->m_TrackperfFnCnt == 0) {
                    // In this case we have 
                    // non-tracked --> IRQ handler
                    // so proceed
                } else if (plgState->m_TrackperfFnCnt > 0) {
                    // In this case we have 
                    // tracked --> IRQ handler
                    // so disable statistics
                    plgState->m_TrackperfFnCnt = 0;
                    return;
                } else {
                    assert (false && "m_TrackperfFnCnt < 0?");
                }
                continue;
            }
            else if (m_TrackperfIRQ == TRACKPERF_IRQ_ALL) {
                // proceed as we may track everything
            } else {
                assert (false && "m_TrackperfIRQ strange value");
            }
        } else if (irq_depth > 1) {
            // should abort early as per previous if statement
            assert (m_TrackperfIRQ == TRACKPERF_IRQ_ONLY_CALLED ||
                    m_TrackperfIRQ == TRACKPERF_IRQ_ALL);
        } else {
            assert (false && "m_TrackperfIRQ < 0?");
        }

        // To track the interrupt handler
        // you need to add it to the list via
        // the s2e_trackperf_fn callout.

        for (i = 0; i < m_TrackperfFn.size(); i++) {
            if (m_TrackperfFnFlags[i] == TRACKPERF_IRQ_HANDLER) {
                continue;
            }

            if (m_TrackperfFnFlags[i] == TRACKPERF_NONTRANSITIVE && j < stack_size - 1) {
                continue;
            }

            std::string tracked = m_TrackperfFn[i];
            assert (
                (m_TrackperfFnFlags[i] == TRACKPERF_TRANSITIVE) ||
                (m_TrackperfFnFlags[i] == TRACKPERF_NONTRANSITIVE && j == stack_size - 1)
                    );
            // Recursive or non-recursive, this part is the same.
            if (cur_fn == tracked) {
                plgState->m_TrackperfFnCnt++;
                helper_ETraceEvent(state, event);
            }
        }
    }

    // MESSAGE_S() << "m_TrackperfFnCnt: " << plgState->m_TrackperfFnCnt << "\n";
}

// accessType:
//  0 = Port
//  1 = MMIO
//  2 = DMA
// address: port or MMIO address
// size: in bytes
// value: value read/written
// isWrite:
//  true = we're doing an I/O write
//  false = we're doing an I/O read
void SymDriveSearcher::onIOMemoryAccess(S2EExecutionState *state,
                                        int accessType,
                                        klee::ref<klee::Expr> address, // or port
                                        klee::ref<klee::Expr> value,
                                        int sizeInBytes, // size in bytes
                                        bool isWrite)
{
    DECLARE_PLUGINSTATE(SymDriveSearcherState, state);

    bool isAddrCste = isa<klee::ConstantExpr>(address);
    bool isValCste = isa<klee::ConstantExpr>(value);

    uint64_t i_pc = state->getPc();
    uint64_t i_virt_address = isAddrCste ? cast<klee::ConstantExpr>(address)->getZExtValue(64) : 0xDEADBEEF;
    uint64_t i_phys_address = 0xDEADBEEF;
    uint64_t i_value = isValCste ? cast<klee::ConstantExpr>(value)->getZExtValue(64) : 0xDEADBEEF;
    uint64_t i_sizeInBytes = sizeInBytes;

    const char *str_accessType;
    switch (accessType) {
        case 0: // Port
            str_accessType = "Port";
            break;
        case 1: // MMIO
            str_accessType = "MMIO";
            break;
        case 2: // DMA
            str_accessType = "DMA";
            break;
        default:
            assert (false && "Specify 0 for port I/O, 1 for MMIO");
    }

    const char *str_isWrite = (isWrite ? "Write" : "Read");

    char s_virt_address[256];
    char s_phys_address[256];
    if (!isAddrCste) {
        sprintf (s_virt_address, "%s", "symbolic");
        sprintf (s_phys_address, "%s", "symbolic");
    } else {
        if (accessType == 1 || accessType == 2) {
            i_phys_address = state->getPhysicalAddress(i_virt_address);
            sprintf (s_phys_address, "0x%lx", i_phys_address);
        } else {
            sprintf (s_phys_address, "portI/O");
        }
        sprintf (s_virt_address, "0x%lx", i_virt_address);
    }

    char s_value[256];
    if (!isValCste) {
        sprintf (s_value, "%s", "symbolic");
    } else {
        sprintf (s_value, "0x%lx", i_value);
    }

    assert (plgState->m_functionCallStackFn.size () == plgState->m_functionCallStackLine.size());
    std::vector<std::string> str_designated_functions;
    int i;
    for (i = (int) plgState->m_functionCallStackFn.size() - 1; i >= 0; i--) {
        std::string current = plgState->m_functionCallStackFn[i];
        if (helper_pointless_function(current)) {
            continue;
        }

        str_designated_functions.push_back(current);
        if (str_designated_functions.size() >= TRACE_HW_OP_NUM_FN) {
            break;
        }
    }

    // Defined in TraceEntries.h
    ExecutionTraceHWAccess e;

    // Zero things out
    memset(&e, 0, sizeof(ExecutionTraceHWAccess));

    e.pc = i_pc;
    for (i = 0; i < TRACE_HW_OP_NUM_FN; i++) {
        std::string last;
        if (i < (int) str_designated_functions.size()) {
            last = str_designated_functions[i];
        } else {
            last = TRACE_HW_OP_NID;
        }
        // Fit name in buffer
        strncpy (e.fn_names[i], last.c_str(), sizeof(e.fn_names[i]));
        // Guarantee null-terminated
        e.fn_names[i][sizeof(e.fn_names[i]) - 1] = 0;
    }
    e.op = (enum TRACE_HW_OP) accessType;
    e.write = isWrite;
    e.virt_address = i_virt_address;
    e.phys_address = i_phys_address;
    e.address_symbolic = !isAddrCste;
    e.value = i_value;
    e.value_symbolic = !isValCste;
    e.size = i_sizeInBytes;
    e.flags = 0;

    assert (e.op == s2e::plugins::TRACE_HW_PORT || \
            e.op == s2e::plugins::TRACE_HW_IOMEM || \
            e.op == s2e::plugins::TRACE_HW_DMA);
    assert (e.write == 0 || e.write == 1);

    std::string access_type = str_accessType;
    std::string read_write = str_isWrite;
    std::string functions;
    int num_fn = 0;
    for (i = TRACE_HW_OP_NUM_FN - 1; i >= 0; i--) {
        if (strcmp (e.fn_names[i], TRACE_HW_OP_NID) == 0) {
            continue;
        }

        if (num_fn > 0) {
            functions += "\t\t";
        }
        functions += e.fn_names[i];
        if (i > 0) {
            functions += " -> ";
        }
        functions += "\n";
        num_fn++;
    }

    MESSAGE_S() << "IOMemoryTracer: " << "\n"
                << "\tPC: " << hexval(e.pc) << "\n"
                << "\tFunction: " << functions
                << "\tAccess Type: " << access_type << "\n"
                << "\tRead/Write: " << read_write << "\n"
                << "\tVirtual Address: " << hexval(e.virt_address)
                    << ", symbolic? " << e.address_symbolic << "\n"
                << "\tPhysical Address: " << hexval(e.phys_address)
                    << ", symbolic? " << e.address_symbolic << "\n"
                << "\tValue: " << e.value
                    << ", symbolic? " << e.value_symbolic << "\n"
                << "\tSize: " << (int) e.size << "\n"
                << "\tFlags: " << (int) e.flags << "\n"
                << "\tState ID: " << state->getID() << "\n";

    m_tracer->writeData(state, &e, sizeof(e), TRACE_HW_ACCESS);

    // Track h/w operation counts for specific function:
    if (helper_should_trackperf(state, 0)) {
        // Record all symbolic I/O assuming it's our driver.
        if (e.op == s2e::plugins::TRACE_HW_PORT) {
            MESSAGE_S() << "Recording PIO operation.\n";
            if (e.write == 0) {
                plgState->m_curTrackPerf[SymDriveSearcherState::PIO_Read]++;
            } else {
                plgState->m_curTrackPerf[SymDriveSearcherState::PIO_Write]++;
            }
        } else if (e.op == s2e::plugins::TRACE_HW_IOMEM) {
            MESSAGE_S() << "Recording MMIO operation.\n";
            if (e.write == 0) {
                plgState->m_curTrackPerf[SymDriveSearcherState::MMIO_Read]++;
            } else {
                plgState->m_curTrackPerf[SymDriveSearcherState::MMIO_Write]++;
            }
        } else if (e.op == s2e::plugins::TRACE_HW_DMA) {
            MESSAGE_S() << "Recording DMA operation.\n";
            if (e.write == 0) {
                plgState->m_curTrackPerf[SymDriveSearcherState::DMA_Read]++;
            } else {
                assert (false && "DMA writes are not currently being recorded?");
                plgState->m_curTrackPerf[SymDriveSearcherState::DMA_Write]++;
            }
        } else {
            assert (false);
        }
    }
}

//Update the metrics
void SymDriveSearcher::onTraceTbEnd(S2EExecutionState* state, uint64_t pc)
{
    //Increment the number of times the current tb is executed
    const ModuleDescriptor *curModule = m_moduleExecutionDetector->getModule(state, pc);
    DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
    if (!curModule) {
        //WARNING() << "GDB screwing up onTraceTb" << "\n";
        //WARNING() << "This is normally an assertion failure." << "\n";
        m_states.erase(state);
        plgState->m_metricValid = false;
        m_states.insert(state);
        return;
    }

    const ModuleDescriptor *md = m_moduleExecutionDetector->getCurrentDescriptor(state);
    uint64_t tbVa = curModule->ToRelative(state->getTb()->pc);
    if (plgState->m_metricValid == false) {
        m_states.erase(state);
        plgState->m_metricValid = true;
        m_states.insert(state);
    }

    if (!md) {
        m_states.erase(state);
        helper_check_state_not_exists(state);
        helper_check_invariants (true);

        m_coveredTbs[*curModule][tbVa]++;
        plgState->m_metric = m_coveredTbs[*curModule][tbVa];
        plgState->m_metric *= state->queryCost < 1 ? 1 : state->queryCost;

        m_states.insert(state);
        helper_check_invariants (true);
        return;
    }

    uint64_t newPc = md->ToRelative(state->getPc());

    TbMap &tbm = m_coveredTbs[*md];
    TbMap::iterator NewTbIt = tbm.find(newPc);
    TbMap::iterator CurTbIt = tbm.find(tbVa);

    bool NextTbIsNew = NewTbIt == tbm.end();
    bool CurTbIsNew = CurTbIt == tbm.end();

    helper_check_invariants(true);
    m_states.erase(state);
    helper_check_state_not_exists(state);
    helper_check_invariants (true);

    /**
     * Update the frequency of the current and next
     * translation blocks
     */
    if (CurTbIsNew) {
      tbm[tbVa] = 1;
    }else {
      (*CurTbIt).second++;
    }

    if (NextTbIsNew && md) {
      tbm[newPc] = 0;
    }

    if (NextTbIsNew) {
      tbm[newPc] = 0;
    }

    plgState->m_metric = tbm[newPc];
    plgState->m_metric *= state->queryCost < 1 ? 1 : state->queryCost;

/*
    MESSAGE() << "[State " << state->getID()
                               << "] - " << md->Name << ", Metric for 0x" << hexval(newPc+md->NativeBase)
                               << " = " << plgState->m_metric
                               << ", priorityChange: " << plgState->m_priorityChange << "\n";
*/

    m_states.insert(state);
    helper_check_invariants (true);
}

void SymDriveSearcher::onTraceTbStart(S2EExecutionState* state, uint64_t pc) {
    const ModuleDescriptor *curModule = NULL, *md = NULL;
    std::string function;
    if (helper_should_trackperf(state, pc, &curModule, &md, &function) == false) {
        return;
    }

    // Trace translation block
    DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
    uint64_t delta = pc - md->LoadBase;
    MESSAGE_S() << "ExecutingBB Mod: " << md->Name
                << ", PC: " << hexval(pc)
                << ", delta: " << hexval(delta)
                << ", FN: " << function
                << "\n";
    plgState->m_curTrackPerf[SymDriveSearcherState::BB]++;

    // Using S2E trace infrastructure too
    helper_ETraceBB (state, pc, delta, function);
    m_lastTbTraced = pc;
}

void SymDriveSearcher::onTraceInstruction(S2EExecutionState* state, uint64_t pc) {
    const ModuleDescriptor *curModule = NULL, *md = NULL;
    std::string function;
    if (helper_should_trackperf(state, pc, &curModule, &md, &function) == false) {
        return;
    }

    // Trace instruction execution
    DECLARE_PLUGINSTATE(SymDriveSearcherState, state);
    uint64_t delta = pc - md->LoadBase;
    MESSAGE_S() << "ExecutingInst Mod: " << md->Name
                << ", PC: " << hexval(pc)
                << ", delta: " << hexval(delta)
                << ", FN: " << function
                << "\n";
    plgState->m_curTrackPerf[SymDriveSearcherState::INST]++;

    // Make sure instr/BB match up.
    if (m_lastTbTraced != 0) {
        assert (pc == m_lastTbTraced);
        m_lastTbTraced = 0;
    }

    helper_ETraceInstr (state, pc, delta, function);
}

void SymDriveSearcher::onTimer()
{
    //Calling this is very expensive and should be done as rarely as possible.
    // llvm::sys::TimeValue curTime = llvm::sys::TimeValue::now();
    // m_currentTime = curTime.seconds();
    m_currentTime++;
}

S2EExecutionState *SymDriveSearcher::selectStateFS(void) {
    if (m_states.size() > 0) {
        S2EExecutionState *es = dynamic_cast<S2EExecutionState*>(*m_states.begin());
        return es;
    } else {
        assert (false);
    }
}

S2EExecutionState *SymDriveSearcher::selectStateMode1(int64_t target_metric) {
    S2EExecutionState *state = NULL;

    if (target_metric == 0 || target_metric == 1) {
        if (m_states.size() > 0) {
            S2EExecutionState *es = dynamic_cast<S2EExecutionState*>(*m_states.begin());
            DECLARE_PLUGINSTATE(SymDriveSearcherState, es);
            if (plgState->m_metricValid &&
                plgState->m_metric < 2) {
                state = es;
            }
        }
    }

    if (state != NULL) {
        return state;
    }

    if (target_metric >= 0 && target_metric < 100) {
        // Find ANY state we track with the target metric
        foreach2(it, m_states.begin(), m_states.end()) {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, *it);
            if (plgState->m_metricValid &&
                plgState->m_metric == target_metric) {
                state = *it;
                break;
            }
        }
    } else if (target_metric == -1) {
        int64_t max_metric = 0;
        // Find a state that has a really high metric
        foreach2(it, m_states.begin(), m_states.end()) {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, *it);
            if (plgState->m_metricValid &&
                plgState->m_metric > max_metric) {
                state = *it;
                max_metric = plgState->m_metric;
            }
        }
    } else if (target_metric == -2) {
        // Find a random state.
        int rnd = rand() % m_states.size();
        int count = 0;
        foreach2(it, m_states.begin(), m_states.end()) {
            if (count == rnd) {
                state = *it;
                break;
            }
            count++;
        }
    } else {
        assert (false);
    }
    return state;
}

S2EExecutionState *SymDriveSearcher::selectStateMode2a(void) {
    assert (m_favorSuccessful == false);
    std::string FunctionRare = "";

    // Find any rare functions
    foreach2(it, m_FunctionCounts.begin(), m_FunctionCounts.end()) {
        if ((*it).second < 3) {
            FunctionRare = (*it).first;
            break;
        }
    }

    if (FunctionRare != "") {
        foreach2(it, m_states.begin(), m_states.end()) {
            DECLARE_PLUGINSTATE(SymDriveSearcherState, *it);
            foreach2(cur_fn_name,
                     plgState->m_functionCallStackFn.begin(),
                     plgState->m_functionCallStackFn.end()) {
                if (*cur_fn_name == FunctionRare) {
                    return *it;
                }
            }
        }
    }

    return NULL;
}

S2EExecutionState *SymDriveSearcher::selectStateMode2b(void) {
#if 0
    S2EExecutionState *state = NULL;

    assert (m_favorSuccessful == false);

    // Added this because of our prioritization scheme:
    // Make sure this code mirrors the onFork path deletion code
    //int most_negative = 0;
    int least_negative = -10; // Rather than INT_MIN should cover typical failure paths in drivers
    //int64_t best_metric = 0x7FFFFFFF;
    int64_t worst_success_metric = 0;

    // Find functions that have poor coverage
    std::string poorestCoverageFunc;
    double ratio = 0;
    double worst_coverage = 2.0;
    foreach2(it, m_BlockCounts.begin(), m_BlockCounts.end()) {
        ratio = (double) (*it).second.blocks_touched.size () /
            (double) (*it).second.total_blocks;
        if (ratio < worst_coverage) {
            poorestCoverageFunc = (*it).first;
            worst_coverage = ratio;
        }
    }

    foreach2(it, m_states.begin(), m_states.end()) {
        DECLARE_PLUGINSTATE(SymDriveSearcherState, *it);
        int64_t plgStateMetric;
        if (plgState->m_metricValid == false) {
            plgStateMetric = 0;
        } else {
            plgStateMetric = plgState->m_metric / plgState->m_driverCallStack;
        }

        if (plgState->m_successPath <= -1 &&
            plgState->m_successPath > least_negative &&
            plgStateMetric < worst_success_metric) {
            state = *it;
            least_negative = plgState->m_successPath;
        }

        /*
          if (backupState1 == NULL) {
          foreach2(cur_fn_name,
          plgState->m_functionCallStackFn.begin(),
          plgState->m_functionCallStackFn.end()) {
          if (*cur_fn_name == poorestCoverageFunc) {
          MESSAGE() << "Poorest Coverage: "
          << poorestCoverageFunc
          << ", ratio"
          << worst_coverage
          << "\n";
          backupState1 = *it;
          break;
          }
          }
          }
        */

        /*
          if (plgState->m_successPath >= 1 &&
          plgStateMetric < 1000 &&
          backupState2 == NULL) {
          // Find a path that has some
          // reasonable properties.
          backupState2 = *it;
          }
        */
    }

    return state;
#endif
    return NULL;
}

// find a state with a target fn on the call stack (m_PrimaryFn)
S2EExecutionState *SymDriveSearcher::selectStateMode3(void) {
    S2EExecutionState *state = NULL;
    int best_primary_fn_count = 0;

    foreach2(it, m_states.begin(), m_states.end()) {
        DECLARE_PLUGINSTATE(SymDriveSearcherState, *it);

        int matched_count = 0;
        foreach2(cur_fn_name,
                 plgState->m_functionCallStackFn.begin(),
                 plgState->m_functionCallStackFn.end()) {
            foreach2(primary_fn_name,
                     m_PrimaryFn.begin(),
                     m_PrimaryFn.end()) {
                if (*cur_fn_name == *primary_fn_name) {
                    matched_count++;
                }
            }

            if (matched_count > best_primary_fn_count) {
                state = *it;
                best_primary_fn_count = matched_count;
            }
        }
    }

    return state;
}

S2EExecutionState *SymDriveSearcher::selectStateMode4(bool greatest) {
    S2EExecutionState *state = NULL;
    int longest_success = 0;

    foreach2(it, m_states.begin(), m_states.end()) {
        DECLARE_PLUGINSTATE(SymDriveSearcherState, *it);
        if (greatest) {
            // Find state with longest success path
            if (plgState->m_successPath > longest_success) {
                longest_success = plgState->m_successPath;
                state = *it;
            }
        } else {
            // Find state with shortest success path
            if (plgState->m_successPath < longest_success) {
                longest_success = plgState->m_successPath;
                state = *it;
            }
        }
    }

    return state;
}

S2EExecutionState *SymDriveSearcher::selectStateMode5(bool greatest) {
    S2EExecutionState *state = NULL;
    int greatest_call_depth = 0;

    foreach2(it, m_states.begin(), m_states.end()) {
        DECLARE_PLUGINSTATE(SymDriveSearcherState, *it);
        if (greatest) {
            // Find state with deepest call stack.
            if (plgState->m_driverCallStack > greatest_call_depth) {
                greatest_call_depth = plgState->m_driverCallStack;
                state = *it;
            }
        } else {
            // Find state with shallowest call stack.
            if (plgState->m_driverCallStack < greatest_call_depth) {
                greatest_call_depth = plgState->m_driverCallStack;
                state = *it;
            }
        }
    }

    return state;
}

klee::ExecutionState& SymDriveSearcher::selectState()
{
    S2EExecutionState *state = NULL;

    if (m_favorSuccessful == true) {
        // Re-execute current state until g_time_budget_fs (favor successful)
        if ((m_currentTime - m_lastStartTime) < g_time_budget_fs && m_lastState != NULL) {
            state = m_lastState;
        }

        if (state == NULL) {
            state = selectStateFS();
        }
    } else {
        // Re-execute current state until g_time_budget_mc (max coverage)
        if ((m_currentTime - m_lastStartTime) < g_time_budget_mc && m_lastState != NULL) {
            state = m_lastState;
        }

        if (state == NULL) {
            // We need to pick the state that's most likely
            // to uncover new code.  Ideas?
            state = selectStateMode1(0); // metric = 1
        }

        if (state == NULL) {
            if (m_selectionMode == 1) {
            } else if (m_selectionMode == 2) {
                state = selectStateMode1(-1); // metric = very high
            } else if (m_selectionMode == 3) {
                state = selectStateMode2a();
            } else if (m_selectionMode == 4) {
                state = selectStateMode3();
            } else if (m_selectionMode == 5) {
                state = selectStateMode4(true);
            } else if (m_selectionMode == 6) {
                state = selectStateMode4(false);
            } else if (m_selectionMode == 7) {
                state = selectStateMode5(true);
            } else if (m_selectionMode == 8) {
                state = selectStateMode5(false);
            } else {
                assert (false);
            }

            m_selectionMode++;
            if (m_selectionMode > 8) {
                m_selectionMode = 1;
            }
        }
    }

    if (state == NULL) {
        state = selectStateMode1(-2); // random state
        assert (state != NULL);
    }

    // If this is true, we're selecting a new state:
    if (state != m_lastState) {
        helper_dump_priorities(state);

        m_lastState = state;
        m_lastStartTime = m_currentTime;
    }

    assert ((state->isActive() == false) ||
            (state->isActive() == true && state == m_lastState));

    return *state;
}

bool SymDriveSearcher::updatePc(S2EExecutionState *es)
{
    const ModuleDescriptor* md = m_moduleExecutionDetector->getCurrentDescriptor(es);
    DECLARE_PLUGINSTATE(SymDriveSearcherState, es);

    if (!md) {
        MESSAGE() << "SymDriveSearcher: state in unknown location" << "\n";
        plgState->m_metricValid = false;
        m_states.insert(es);
        return false;
    }

    //Retrieve the next program counter of the state
    uint64_t absNextPc = computeTargetPc(es); // XXX: fix me

    if (!absNextPc) {
        MESSAGE() << "SymDriveTBSearcher: could not determine next pc" << "\n";
        //Could not determine next pc
        plgState->m_metricValid = false;
        m_states.insert(es);
        return false;
    }

    //If not covered, add the forked state to the wait list
    plgState->m_metricValid = true;
    plgState->m_metric = m_coveredTbs[*md][md->ToRelative(absNextPc)];
    m_states.insert(es);
    helper_check_invariants (false); // Invariant may not hold as we're in the middle of an update.

    MESSAGE() << "[StateX " << es->getID()
              << "] - " << md->Name << ", Metric for 0x" << hexval(es->getPc())
              << " = " << plgState->m_metric
              << ", priorityChange: " << plgState->m_priorityChange << "\n";

    return true;
}

void SymDriveSearcher::update(klee::ExecutionState *state,
                              const std::set<klee::ExecutionState*> &addedStates,
                              const std::set<klee::ExecutionState*> &removedStates)
{
    // MESSAGE() << "update! " << addedStates.size() << ", " << removedStates.size() << "\n";

    foreach2(it, addedStates.begin(), addedStates.end()) { // Swapped order
        S2EExecutionState *es = dynamic_cast<S2EExecutionState*>(*it);
        updatePc(es);
    }

    foreach2(it, removedStates.begin(), removedStates.end()) {
        S2EExecutionState *es = dynamic_cast<S2EExecutionState*>(*it);
        // Invariant may not hold as we're in the middle of an update.
        helper_check_invariants (false);

        if (m_dumpiomap_enabled) {
            // Print out the names + locations of I/O read operations.
            helper_dump_io_map(es);
        }

        m_states.erase(es);
        if (es == m_lastState) {
            m_lastState = NULL;
        }
        helper_check_state_not_exists(es);
        helper_check_invariants (false);
    }

    helper_check_invariants (false);
}

bool SymDriveSearcher::empty()
{
    return m_states.empty();
}

bool SymDriveSearcher::establishIOMap(std::string tag) const {
    DECLARE_PLUGINSTATE(SymDriveSearcherState, g_s2e_state);
    plgState->m_ioMap[tag] = helper_driver_call_stack(plgState);
    return true;
}

extern "C" {

static bool io_map_callback (std::string tag, void *opaque) {
    SymDriveSearcher *searcher = static_cast<SymDriveSearcher *>(opaque);
    return searcher->establishIOMap(tag);
}

}

///////////////////////////////////////////////////////////////////////////////

SymDriveSearcherState::SymDriveSearcherState()
{

}

SymDriveSearcherState::SymDriveSearcherState(S2EExecutionState *s, Plugin *p)
{
    m_metric = 0;
    m_metricValid = true;
    m_priorityChange = 0;
    m_successPath = 0;
    m_plugin = static_cast<SymDriveSearcher*>(p);

    m_driverCallStack = 0;
    m_TrackperfFnCnt = 0;

    int i;
    for (i = 0; i < SymDriveSearcherState::TRACKPERF_COUNT; i++) {
        m_curTrackPerf[i] = 0;
    }
}

SymDriveSearcherState::~SymDriveSearcherState()
{
}

PluginState *SymDriveSearcherState::clone() const
{
    size_t i;
    SymDriveSearcherState *retval = new SymDriveSearcherState(*this);
    assert (this->m_metric == retval->m_metric);
    assert (this->m_metricValid == retval->m_metricValid);
    assert (this->m_priorityChange == retval->m_priorityChange);
    assert (this->m_successPath == retval->m_successPath);
    assert (this->m_driverCallStack == retval->m_driverCallStack);

    assert (this->m_TrackPerf.size() == retval->m_TrackPerf.size());

    for (i = 0; i < TRACKPERF_COUNT; i++) {
        assert (this->m_curTrackPerf[i] == retval->m_curTrackPerf[i]);
    }

    assert (this->m_loopStates.size() == retval->m_loopStates.size());
    assert (this->m_functionCallStackFn.size() == retval->m_functionCallStackFn.size());
    assert (this->m_functionCallStackLine.size() == retval->m_functionCallStackLine.size());
    for (i = 0; i < this->m_loopStates.size(); i++) {
        assert (this->m_loopStates[i] == retval->m_loopStates[i]);
    }

    // General sanity
    assert (this->m_functionCallStackFn.size() == this->m_functionCallStackLine.size());
    return retval;
}

PluginState *SymDriveSearcherState::factory(Plugin *p, S2EExecutionState *s)
{
    SymDriveSearcherState *ret = new SymDriveSearcherState(s, p);
    return ret;
}

//////////////////////////////
// Sorting
//////////////////////////////
SymDriveSorter::SymDriveSorter() {
    p = NULL;
}

bool SymDriveSorter::operator()(const S2EExecutionState *s1, const S2EExecutionState *s2) const {
    // return true if s1 is higher priority
    // return false if s2 is higher priority

    const SymDriveSearcherState *p1 = static_cast<SymDriveSearcherState*>
        (p->getPluginState(const_cast<S2EExecutionState*>(s1), &SymDriveSearcherState::factory));
    const SymDriveSearcherState *p2 = static_cast<SymDriveSearcherState*>
        (p->getPluginState(const_cast<S2EExecutionState*>(s2), &SymDriveSearcherState::factory));

    if (s1 == s2) {
        assert (p1 == p2);
    }
    if (s1 != s2) {
        assert (p1 != p2);
    }
    if (p1 == p2) {
        assert (s1 == s2);
    }
    if (p1 != p2) {
        assert (s1 != s2);
    }

    if (p->m_favorSuccessful == true) {
        // ignore metric
        if (p1->m_priorityChange > p2->m_priorityChange) {
            return true;
        } else if (p1->m_priorityChange == p2->m_priorityChange) {
            return s1->getID() < s2->getID();
        } else {
            return false;
        }
    } else {
        // metric is more important now
        if (p1->m_metricValid == true && p2->m_metricValid == false) {
            return true;
        } else if (p1->m_metricValid == false && p2->m_metricValid == true) {
            return false;
        } else {
            if (p1->m_metric < p2->m_metric) {
                return true;
            } else if (p1->m_metric == p2->m_metric) {
                return s1->getID() < s2->getID();
            } else {
                return false;
            }
        }
    }
}

} // namespace plugins
} // namespace s2e
