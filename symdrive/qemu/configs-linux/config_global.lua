-- Update the config_global for FreeBSD too

s2e = {
   kleeArgs = {
      "--use-random-path",
      "--use-batching-search",
      "--use-cex-cache=true",
      "--use-cache=true",
      "--use-fast-cex-solver=true",
      "--max-stp-time=10",
      "--use-expr-simplifier=true",
      "--print-expr-simplifier=false",
      "--flush-tbs-on-state-switch=false",
      "--print-mode-switch=false",
      "--print-llvm-instructions=false",
      "--use-fast-helpers=true"
   }
}

plugins = {
   "SymbolicHardware",
   "ExecutionTracer",
   "FunctionMonitor",
   "BaseInstructions",

   "RawMonitor",
   "ModuleTracer",
   "ModuleExecutionDetector",
   "SymDriveTranslationBlockTracer",
--   "OrigMaxTbSearcher",
--   "MaxTbSearcher",
   "SymDriveSearcher",
}

pluginsConfig = {}

pluginsConfig.ModuleExecutionDetector = {
   trackAllModules = true,
   configureAllModules = true,
}
