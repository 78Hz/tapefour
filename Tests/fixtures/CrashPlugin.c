/* A VST3 module that crashes the moment a host asks it for anything.
   ScanTest scans it to prove TapeFour survives a crashing plug-in. */
#if defined(_WIN32)
 #define EXPORT __declspec(dllexport)
#else
 #define EXPORT __attribute__((visibility("default")))
#endif

static void crash (void)
{
    volatile int* p = (volatile int*) 0;
    *p = 1;
}

EXPORT int InitDll (void)               { crash(); return 1; }
EXPORT int ExitDll (void)               { return 1; }
EXPORT int ModuleEntry (void* h)        { (void) h; crash(); return 1; }
EXPORT int ModuleExit (void)            { return 1; }
EXPORT int bundleEntry (void* b)        { (void) b; crash(); return 1; }
EXPORT int bundleExit (void)            { return 1; }
EXPORT void* GetPluginFactory (void)    { crash(); return 0; }
