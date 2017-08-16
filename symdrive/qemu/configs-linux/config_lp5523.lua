dofile "./configs-linux/config_global.lua"

pluginsConfig.SymbolicHardware = {
     pcntpci5f = {
        id="lp5523f",
        type="pci",
        vid=0x9892,
        pid=0x9893,
        classCode=0,
        revisionId=0x0,
        interruptPin=1,
        resources={
           -- isIo = true means port I/O
           -- isIo = false means I/O memory
           r0 = { isIo=false, size=0x100000, isPrefetchable=false},
           r1 = { isIo=false, size=0x100000, isPrefetchable=false},
           r2 = { isIo=false, size=0x100000, isPrefetchable=false},
        }
    }
}

pluginsConfig.RawMonitor = {
   kernelStart = 0xC000000,

   test_pci = {
      name = "lp5523_stub",
      size = 0,
      start = 0,
      nativebase = 0,
      delay = false,        -- delay load?
      kernelmode = true,
      primaryModule = true
   },

   test_framework_lp5523 = {
      name = "test_framework_lp5523",
      size = 0,
      start = 0,
      nativebase = 0,
      delay = false,        -- delay load?
      kernelmode = true,
      primaryModule = false
   }
}

pluginsConfig.SymDriveSearcher = {
   test_pci = {
      moduleName = "lp5523_stub",
      moduleDir = "/home/mjr/s2e/symdrive/test/lp5523"
   },

   test_framework = {
      moduleName = "test_framework_lp5523",
      moduleDir = "/home/mjr/s2e/symdrive/test/test_framework_lp5523"
   }
}
