#pragma once

#include <stdint.h>

#if defined(__vita__) || defined(__psp2__) || defined(OPENSHACCG_USE_VITASDK_HEADERS)
#include <psp2/shacccg.h>
#else
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SceShaccCgCompileOptions SceShaccCgCompileOptions;
typedef struct SceShaccCgSourceFile SceShaccCgSourceFile;
typedef struct SceShaccCgSourceLocation SceShaccCgSourceLocation;
typedef const void *SceShaccCgParameter;

typedef enum SceShaccCgDiagnosticLevel {
    SCE_SHACCCG_DIAGNOSTIC_LEVEL_INFO = 0,
    SCE_SHACCCG_DIAGNOSTIC_LEVEL_WARNING = 1,
    SCE_SHACCCG_DIAGNOSTIC_LEVEL_ERROR = 2
} SceShaccCgDiagnosticLevel;

typedef enum SceShaccCgTargetProfile {
    SCE_SHACCCG_PROFILE_VP = 0,
    SCE_SHACCCG_PROFILE_FP = 1
} SceShaccCgTargetProfile;

typedef enum SceShaccCgCallbackDefaults {
    SCE_SHACCCG_SYSTEM_FILES = 0,
    SCE_SHACCCG_TRIVIAL = 1
} SceShaccCgCallbackDefaults;

typedef enum SceShaccCgLocale {
    SCE_SHACCCG_ENGLISH = 0,
    SCE_SHACCCG_JAPANESE = 1
} SceShaccCgLocale;

struct SceShaccCgSourceFile {
    const char *fileName;
    const char *text;
    uint32_t size;
};

struct SceShaccCgSourceLocation {
    const SceShaccCgSourceFile *file;
    uint32_t lineNumber;
    uint32_t columnNumber;
};

typedef SceShaccCgSourceFile *(*SceShaccCgCallbackOpenFile)(
    const char *, const SceShaccCgSourceLocation *, const SceShaccCgCompileOptions *, const char **);
typedef void (*SceShaccCgCallbackReleaseFile)(const SceShaccCgSourceFile *, const SceShaccCgCompileOptions *);
typedef const char *(*SceShaccCgCallbackLocateFile)(
    const char *, const SceShaccCgSourceLocation *, uint32_t, const char *const *, const SceShaccCgCompileOptions *, const char **);
typedef const char *(*SceShaccCgCallbackAbsolutePath)(
    const char *, const SceShaccCgSourceLocation *, const SceShaccCgCompileOptions *);
typedef void (*SceShaccCgCallbackReleaseFileName)(const char *, const SceShaccCgCompileOptions *);
typedef int32_t (*SceShaccCgCallbackFileDate)(
    const SceShaccCgSourceFile *, const SceShaccCgSourceLocation *, const SceShaccCgCompileOptions *, int64_t *, int64_t *);

typedef struct SceShaccCgCallbackList {
    SceShaccCgCallbackOpenFile openFile;
    SceShaccCgCallbackReleaseFile releaseFile;
    SceShaccCgCallbackLocateFile locateFile;
    SceShaccCgCallbackAbsolutePath absolutePath;
    SceShaccCgCallbackReleaseFileName releaseFileName;
    SceShaccCgCallbackFileDate fileDate;
} SceShaccCgCallbackList;

struct SceShaccCgCompileOptions {
    const char *mainSourceFile;
    SceShaccCgTargetProfile targetProfile;
    const char *entryFunctionName;
    uint32_t searchPathCount;
    const char *const *searchPaths;
    uint32_t macroDefinitionCount;
    const char *const *macroDefinitions;
    uint32_t includeFileCount;
    const char *const *includeFiles;
    uint32_t suppressedWarningsCount;
    const uint32_t *suppressedWarnings;
    SceShaccCgLocale locale;
    int32_t useFx;
    int32_t noStdlib;
    int32_t optimizationLevel;
    int32_t useFastmath;
    int32_t useFastprecision;
    int32_t useFastint;
    int field_48;
    int32_t warningsAsErrors;
    int32_t performanceWarnings;
    int32_t warningLevel;
    int32_t pedantic;
    int32_t pedanticError;
    int field_60;
    int field_64;
};

typedef struct SceShaccCgDiagnosticMessage {
    SceShaccCgDiagnosticLevel level;
    uint32_t code;
    const SceShaccCgSourceLocation *location;
    const char *message;
} SceShaccCgDiagnosticMessage;

typedef struct SceShaccCgCompileOutput {
    const uint8_t *programData;
    uint32_t programSize;
    int32_t diagnosticCount;
    const SceShaccCgDiagnosticMessage *diagnostics;
} SceShaccCgCompileOutput;

int sceShaccCgInitializeCompileOptions(SceShaccCgCompileOptions *options);
const SceShaccCgCompileOutput *sceShaccCgCompileProgram(
    const SceShaccCgCompileOptions *options, const SceShaccCgCallbackList *callbacks, int unk);
int sceShaccCgSetDefaultAllocator(void *(*malloc_cb)(unsigned int), void (*free_cb)(void *));
void sceShaccCgInitializeCallbackList(SceShaccCgCallbackList *callbacks, SceShaccCgCallbackDefaults defaults);
void sceShaccCgDestroyCompileOutput(const SceShaccCgCompileOutput *output);
void sceShaccCgReleaseCompiler(void);
const char *sceShaccCgGetVersionString(void);

#ifdef __cplusplus
}
#endif
#endif
