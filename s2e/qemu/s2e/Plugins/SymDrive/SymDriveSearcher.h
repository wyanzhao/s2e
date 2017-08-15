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

#ifndef S2E_PLUGINS_SYMDRIVESEARCHER_H
#define S2E_PLUGINS_SYMDRIVESEARCHER_H

#include <s2e/Plugin.h>
#include <s2e/Plugins/CorePlugin.h>
#include <s2e/Plugins/ModuleExecutionDetector.h>
#include <s2e/S2EExecutionState.h>

#include "../ExecutionTracers/ExecutionTracer.h" // tracing
#include "../SymbolicHardware.h" // tracing
#include "../RawMonitor.h" // tracing

#include "MCoverageBasics.h" // basic blocks

#include <klee/Searcher.h>

#include <vector>

namespace s2e {

class S2EExecutor;

namespace plugins {

template <typename T> std::string number_to_string(T number)
{
    return dynamic_cast<std::stringstream *> (&(std::stringstream() << number))->str();
}

class SymDriveSearcher;
class SymDriveSearcherState;

struct SymDriveModuleInfo {
    SymDriveModuleInfo() {
        m_moduleName = "";
        m_moduleDir = "";
        m_numGaps = 0;
        m_bytesInGaps = 0;
        m_bytesTotal = 0;
    }
    std::string m_moduleName;
    std::string m_moduleDir;
    s2etools::BasicBlocks m_allBbs;
    s2etools::Functions m_functions;
    s2etools::BBToFunction m_AddrToFunction;
    std::map<uint64_t, bool> m_AddrValid;
    int m_numGaps;
    int m_bytesInGaps;
    int m_bytesTotal;
};

class SymDriveSorter {
    friend class s2e::plugins::SymDriveSearcher;
  private:
    SymDriveSearcher *p;
  public:
    SymDriveSorter();
    bool operator()(const S2EExecutionState *s1, const S2EExecutionState *s2) const;
};

typedef std::set<S2EExecutionState*, SymDriveSorter> StateSet;

class SymDriveSearcherState: public PluginState
{
    friend class s2e::plugins::SymDriveSorter;
  public:
    enum RECORDED_OPERATIONS {
        BB,
        INST,
        PIO_Read,
        PIO_Write,
        MMIO_Read,
        MMIO_Write,
        DMA_Read,
        DMA_Write,
        TRACKPERF_COUNT
    };

  private:
    int64_t m_metric;
    bool m_metricValid;
    int m_priorityChange;
    int m_successPath;
    SymDriveSearcher *m_plugin;
    std::vector<unsigned int> m_loopStates;
    int m_driverCallStack;
    std::map<std::string, std::string> m_ioMap;

    std::vector<std::string> m_functionCallStackFn;
    std::vector<int> m_functionCallStackLine;

    //
    // Performance tracking
    //
    std::vector<int> m_TrackPerf;
    int m_TrackperfFnCnt; // > 0 if we've entered an interesting fn.

    int m_curTrackPerf[TRACKPERF_COUNT];
    std::vector<int> m_prevTrackPerf[TRACKPERF_COUNT];

  public:
    SymDriveSearcherState();
    SymDriveSearcherState(S2EExecutionState *s, Plugin *p);
    virtual ~SymDriveSearcherState();
    virtual PluginState *clone() const;
    static PluginState *factory(Plugin *p, S2EExecutionState *s);

    friend class SymDriveSearcher;
};

class SymDriveSearcher : public Plugin, public klee::Searcher
{
    S2E_PLUGIN
    friend class s2e::plugins::SymDriveSorter;
  public:
    //Maps a translation block address to the number of times it was executed
    typedef std::map<uint64_t, uint64_t> TbMap;
    typedef std::map<ModuleDescriptor, TbMap, ModuleDescriptor::ModuleByName > TbsByModule;

  SymDriveSearcher(S2E* s2e): Plugin(s2e) {}
    void initialize();

  private:
    // basic blocks
    void initializeBB (void);
    void initSection(const std::string &cfgKey, const std::string &svcId);
    void ReadBBList(SymDriveModuleInfo &module);
    void BBValidate(const SymDriveModuleInfo &module) const;
    std::string AddrToFunction(const ModuleDescriptor *md, uint64_t address) const;
    std::string AddrIsValid (const ModuleDescriptor *md, uint64_t address) const;
    
    void initializeSearcher();
    uint64_t computeTargetPc(S2EExecutionState *s);
    void onModuleTranslateBlockEnd(
        ExecutionSignal *signal,
        S2EExecutionState* state,
        const ModuleDescriptor &,
        TranslationBlock *tb,
        uint64_t endPc,
        bool staticTarget,
        uint64_t targetPc);
    void onModuleTranslateBlockStart(
        ExecutionSignal *signal,
        S2EExecutionState* state,
        const ModuleDescriptor &,
        TranslationBlock *tb,
        uint64_t endPc);
    void onTranslateInstructionStart(
        ExecutionSignal *signal,
        S2EExecutionState* state,
        TranslationBlock *tb,
        uint64_t pc);
    void onFork(S2EExecutionState *originalState,
                const std::vector<S2EExecutionState*>& newStates,
                const std::vector<klee::ref<klee::Expr> >& newConditions);
    void onCustomInstruction   (S2EExecutionState *state, uint64_t opcode);

    // Opcodes
    // If you update this, be sure to update root_annot_dri.ml
    // and s2e.h
#define SYMDRIVE_OPCODE_ARGS S2EExecutionState *state, SymDriveSearcherState* plgState, int line
    void s2e_prioritize           (SYMDRIVE_OPCODE_ARGS);
    void s2e_deprioritize         (SYMDRIVE_OPCODE_ARGS);
    void s2e_loop_before          (SYMDRIVE_OPCODE_ARGS);
    void s2e_loop_body            (SYMDRIVE_OPCODE_ARGS);
    void s2e_loop_after           (SYMDRIVE_OPCODE_ARGS);
    void s2e_concretize_kill      (SYMDRIVE_OPCODE_ARGS);
    void s2e_concretize_all       (SYMDRIVE_OPCODE_ARGS);
    void s2e_kill_all_others      (SYMDRIVE_OPCODE_ARGS);
    void s2e_driver_call_stack    (SYMDRIVE_OPCODE_ARGS);
    void s2e_favor_successful     (SYMDRIVE_OPCODE_ARGS);
    // hole
    void s2e_reset_priorities     (SYMDRIVE_OPCODE_ARGS);
    // hole
    void s2e_enable_tracing       (SYMDRIVE_OPCODE_ARGS);
    void s2e_disable_tracing      (SYMDRIVE_OPCODE_ARGS);
    void s2e_enterexit_function   (SYMDRIVE_OPCODE_ARGS, bool enter); // helper
    void s2e_enter_function       (SYMDRIVE_OPCODE_ARGS);
    void s2e_exit_function        (SYMDRIVE_OPCODE_ARGS);
    void s2e_is_symbolic_symdrive (SYMDRIVE_OPCODE_ARGS);
    void s2e_success_path         (SYMDRIVE_OPCODE_ARGS);
    void s2e_enter_block          (SYMDRIVE_OPCODE_ARGS);
    void s2e_primary_fn           (SYMDRIVE_OPCODE_ARGS);
    void s2e_enable_trackperf     (SYMDRIVE_OPCODE_ARGS);
    void s2e_disable_trackperf    (SYMDRIVE_OPCODE_ARGS);
    void s2e_trackperf_fn         (SYMDRIVE_OPCODE_ARGS);
    void s2e_io_region            (SYMDRIVE_OPCODE_ARGS);
#undef SYMDRIVE_OPCODE_ARGS

    // Helpers
    void helper_check_invariants (bool full_checks);
    void helper_check_state_not_exists (S2EExecutionState *state) const;
    void helper_stop_rescheduling (void) const;
    void helper_dump_priorities (S2EExecutionState *state) const;
    bool helper_pointless_function(std::string fn) const;
    std::string helper_driver_call_stack(const SymDriveSearcherState *plgState) const;
    void helper_ETraceInstr (S2EExecutionState *state, uint64_t pc, uint64_t delta, std::string fn) const;
    void helper_ETraceBB (S2EExecutionState *state, uint64_t pc, uint64_t delta, std::string fn) const;
    void helper_ETraceEvent (S2EExecutionState *state, int event) const;
    void helper_ETraceSuccess (S2EExecutionState *state, std::string fn, uint64_t success) const;
    void helper_dump_io_map (S2EExecutionState *state) const;
    std::string helper_dump_allperf (S2EExecutionState *state) const;
    std::string helper_dump_perf (S2EExecutionState *state,
                               std::vector<int> &array,
                               std::string name) const;
    std::string helper_dump_trackperf (S2EExecutionState *state,
                                    std::vector<int> &array) const;
    bool helper_should_trackperf(S2EExecutionState *state,
                              uint64_t pc,
                              const ModuleDescriptor **curModule = NULL,
                              const ModuleDescriptor **md = NULL,
                              std::string *function = NULL) const;
    void helper_perf_store(S2EExecutionState *state,
                        SymDriveSearcherState *plgState);
    void helper_perf_reset(S2EExecutionState *state,
                        SymDriveSearcherState *plgState);
    int helper_readCallId(S2EExecutionState *state,
                       SymDriveSearcherState *plgState);
    void helper_push_driver_call_stack (S2EExecutionState *state,
                                     SymDriveSearcherState *plgState,
                                     std::string fn,
                                     int line);
    void helper_pop_driver_call_stack  (S2EExecutionState *state,
                                     SymDriveSearcherState *plgState,
                                     std::string fn,
                                     int line);
    void helper_push_pop_trackperf (S2EExecutionState *state,
                                 SymDriveSearcherState *plgState,
                                 int event);

    // Tracing
    void onIOMemoryAccess(S2EExecutionState *state,
                          int accessType,
                          klee::ref<klee::Expr> address,
                          klee::ref<klee::Expr> value,
                          int sizeInBytes,
                          bool isWrite);

    void onTraceTbEnd(S2EExecutionState* state, uint64_t pc);
    void onTraceTbStart(S2EExecutionState* state, uint64_t pc);
    void onTraceInstruction(S2EExecutionState* state, uint64_t pc);
    void onTimer(void);

    // State selection
    S2EExecutionState *selectStateFS(void);
    S2EExecutionState *selectStateMode1(int64_t target_metric);
    S2EExecutionState *selectStateMode2a(void);
    S2EExecutionState *selectStateMode2b(void);
    S2EExecutionState *selectStateMode3(void);
    S2EExecutionState *selectStateMode4(bool greatest);
    S2EExecutionState *selectStateMode5(bool greatest);
  public:
    virtual klee::ExecutionState& selectState();

  private:
    // Inherited member functions
    bool updatePc(S2EExecutionState *es);
  public:
    virtual void update(klee::ExecutionState *current,
                        const std::set<klee::ExecutionState*> &addedStates,
                        const std::set<klee::ExecutionState*> &removedStates);

    virtual bool empty();

    // Sets up a mapping between I/O tag and driver call stack.
    bool establishIOMap (std::string tag) const;
    
    ////////////////////////////////////////////////////////
    // Member variables
    ////////////////////////////////////////////////////////
  private:
    static const int PRIORITY_FIDDLE = 1000;
    static const int PRIORITY_YIELD = 50000;
    static const int PRIORITY_EXTREME = 2000000000;

    // Module execution
    ModuleExecutionDetector *m_moduleExecutionDetector;
    bool m_searcherInited;
    TbsByModule m_coveredTbs;

    // State management
    SymDriveSorter m_sorter;
    StateSet m_states;
    typedef std::map<int, int> IntMap;
    // static const int MAX_PENALTY = 128;
    static const int MAX_PENALTY = 10;
    IntMap m_currentPenalty;
    IntMap m_loopCount;
    std::map<std::string, int> m_FunctionCounts;
    typedef struct {
        std::string function;
        int total_blocks;
        std::map<int, bool> blocks_touched;
    } CBlocks;
    std::map<std::string, CBlocks> m_BlockCounts;
    std::vector<std::string> m_PrimaryFn;

    // Scheduling
    bool m_favorSuccessful;
    uint64_t m_currentTime;
    uint64_t m_lastStartTime;
    S2EExecutionState *m_lastState;

    // Selection mode
    int m_selectionMode;

    // Tracing / Performance analysis
    int m_dumpiomap_enabled;
    sigc::connection m_memoryMonitor;
    ExecutionTracer *m_tracer;
    RawMonitor *m_mon;
    
    TranslationBlock *m_tb; // instruction / TB tracing
    sigc::connection m_tbConnection;
    uint64_t m_lastTbTraced;
    std::vector<std::string> m_TrackperfFn;
    std::vector<int> m_TrackperfFnFlags;
    int m_TrackperfIRQ;
        
    // Symbolic hardware
    SymbolicHardware *m_hw;

    // Trackperf:
    // Update s2e.h if this changes
    // Update MTbTrace if this changes
    // Update helper_dump_trackperf if this changes
    const static int PAUSE_PP = 11;
    const static int CONTINUE_PP = 12; // Return to previous state

    const static int PAUSE_STUB = 21;
    const static int CONTINUE_STUB = 22;

    const static int PAUSE_IRQ = 31;
    const static int CONTINUE_IRQ = 32;

    const static int START_AUTO = 40;
    const static int PAUSE_AUTO = 41;
    const static int CONTINUE_AUTO = 42;
    const static int STOP_AUTO = 43;
    const static int DISCARD_AUTO = 44;

    const static int START_MANUAL = 50;
    const static int PAUSE_MANUAL = 51;
    const static int CONTINUE_MANUAL = 52;
    const static int STOP_MANUAL = 53;
    const static int DISCARD_MANUAL = 54;

    // Used only by helper_push_driver_call_stack / pop_driver_call_stack
    // Used only for tracing events.
    const static int START_FN = 60;
    const static int STOP_FN = 61;

    const static int TRACKPERF_IRQ_NONE = 1000; // Handler + called functions not tracked
    const static int TRACKPERF_IRQ_ONLY_CALLED = 1001; // Handler not tracked, called functions maybe
    const static int TRACKPERF_IRQ_ALL = 1002; // Handler + called functions tracked

    // Used for trackperf_fn / push_pop_trackperf
    const static int TRACKPERF_NONTRANSITIVE = 0;
    const static int TRACKPERF_TRANSITIVE = 1;
    const static int TRACKPERF_IRQ_HANDLER = 1000; // See root_annot_dri.ml mk_register_irq_stmts

    //////////////////////// Trackperf

    // Basic blocks:
    std::vector<SymDriveModuleInfo> m_Modules;

    friend class SymDriveSearcherState;
};

} // namespace plugins
} // namespace s2e

#endif
