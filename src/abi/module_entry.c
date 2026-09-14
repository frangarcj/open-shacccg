#ifdef __vita__
#include <psp2/kernel/modulemgr.h>

int module_start(SceSize argc, const void *args) {
    (void)argc; (void)args;
    return SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize argc, const void *args) {
    (void)argc; (void)args;
    return SCE_KERNEL_STOP_SUCCESS;
}
#else
int module_start(unsigned int argc, const void *args) { (void)argc; (void)args; return 0; }
int module_stop(unsigned int argc, const void *args) { (void)argc; (void)args; return 0; }
#endif
