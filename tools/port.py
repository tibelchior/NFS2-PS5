#!/usr/bin/env python3
"""Copy repo/src -> work/src and apply the 64-bit patches (guest addresses stay <2 GB via lowmem.c)."""
import re, shutil, sys, os
here = os.path.dirname(os.path.abspath(__file__)) + "/.."
src, dst = here + "/repo/src", here + "/work/src"
shutil.rmtree(dst, ignore_errors=True); shutil.copytree(src, dst, ignore=shutil.ignore_patterns("Android", "Windows"))
def rw(path, fn):
    s = open(path).read(); s2 = fn(s); open(path, "w").write(s2)
def cpp(s):
    s = s.replace("#if !defined(__WORDSIZE) || (__WORDSIZE != 32)", "#if 0")
    s = s.replace("(int32_t)", "(int32_t)(intptr_t)")
    s = s.replace('ASSERT_SIZE(4, val);\n\t\tesp -= 4;\n\t\tto32i(esp) = (int32_t)(intptr_t)val;',
                  'static_assert(sizeof(val) == 4 || is_pointer<T>::value, "push32");\n\t\tesp -= 4;\n\t\tto32i(esp) = (int32_t)(intptr_t)val;')
    # wrapper entry points: pointer-sized args (sign-extended from low guest addresses)
    s = re.sub(r'(extern "C" int32_t \w+\()([^)]*)\)', lambda m: m.group(1) + m.group(2).replace("int32_t arg", "intptr_t arg") + ")", s)
    s = re.sub(r'(fscanf_wrap\([^;]*?)(\*\(int32_t \*\)\(esp \+ 8\))', r'\1(intptr_t)\2', s)
    return s
rw(dst + "/Cpp/NFS2SE.cpp", cpp)
rw(dst + "/EAcsnd.c", lambda s: s.replace("wrap_regparm2(audio_game_thread, getSamples, a, b)", "wrap_regparm2(audio_game_thread, getSamples, (int32_t)(intptr_t)(a), b)"))
rw(dst + "/EAcsnd.c", lambda s: s.replace("int16_t samples[256 * CHN_CNT];", "static int16_t samples[256 * CHN_CNT];"))
rw(dst + "/DInput.c", lambda s: s.replace("wrap_stdcall2_ret(dinput_game_thread, callback, &deviceInstance, ref)", "wrap_stdcall2_ret(dinput_game_thread, callback, (int32_t)(intptr_t)&deviceInstance, (int32_t)(intptr_t)ref)")
                                  .replace("DIDEVICEINSTANCEA deviceInstance = {0};", "static DIDEVICEINSTANCEA deviceInstance; memset(&deviceInstance, 0, sizeof deviceInstance);"))
rw(dst + "/User32.c", lambda s: s.replace("(void *)msg->wParam", "(void *)(uintptr_t)msg->wParam").replace("(void *)msg->lParam", "(void *)(uintptr_t)msg->lParam")
                                  .replace("(uint32_t)event.user.data1", "(uint32_t)(uintptr_t)event.user.data1").replace("(uint32_t)event.user.data2", "(uint32_t)(uintptr_t)event.user.data2"))
rw(dst + "/Wrapper.c", lambda s: s.replace("signal(SIGSEGV, signal_handler);", "if (!getenv(\"NFS_NOSIG\")) signal(SIGSEGV, signal_handler);"))
rw(dst + "/Wrapper.c", lambda s: s.replace("return vsprintf(s, fmt, arg);", "extern int guest_vsprintf(char *, const char *, const uint32_t *); return guest_vsprintf(s, fmt, (const uint32_t *)arg);"))
# guest-visible Glide structs must keep their 32-bit layout (pointer members are 4 bytes in the game)
rw(dst + "/Glide2x.h", lambda s: s.replace("    void               *lfbPtr;", "    uint32_t           lfbPtr;")
                                  .replace("    void              *data;\n} GrTexInfo;", "    uint32_t          data;\n} GrTexInfo;"))
rw(dst + "/Glide2x/OpenGL2.c", lambda s: s.replace("info->lfbPtr = g_lfb = (uint8_t *)malloc(640*480*2);", "g_lfb = (uint8_t *)malloc(640*480*2); info->lfbPtr = (uint32_t)(uintptr_t)g_lfb;")
                                         .replace("(uint16_t *)info->data;", "(uint16_t *)(uintptr_t)info->data;"))
# guest variables holding pointers are 4 bytes wide
rw(dst + "/Timer.c", lambda s: s.replace("#define dword_4DDA70 (*dword_4DDA70)", "#define dword_4DDA70 ((Event *)(uintptr_t)*(uint32_t *)dword_4DDA70)")
                               .replace("#define dword_5637CC (*dword_5637CC)", "#define dword_5637CC ((Event *)(uintptr_t)*(uint32_t *)dword_5637CC)")
                               .replace("#define dword_5637D8 (*dword_5637D8)", "#define dword_5637D8 ((Event *)(uintptr_t)*(uint32_t *)dword_5637D8)"))
# DirectInput objects are read by the 32-bit game: vtable slots are 4 bytes, object slots are written as 32-bit
def dih(s): return re.sub(r"uint32_t \(STDCALL \*(\w+)\)\([^;]*\);", r"uint32_t \1;", s)
rw(dst + "/DInput.h", dih)
def dic(s):
    s = re.sub(r"(\w+->\w+) = (WRAP_NAME\(\w+\));", r"\1 = (uint32_t)(uintptr_t)\2;", s)
    s = re.sub(r"\*(\w+) = malloc\(sizeof\(void \*\)\);\s*\*\*\1 = (\w+);", r"{ void **slot_ = malloc(sizeof(void *)); *slot_ = \2; *(uint32_t *)\1 = (uint32_t)(uintptr_t)slot_; }", s)
    return s
rw(dst + "/DInput.c", dic)
# structs the game allocates itself (guest layout): no 8-byte pointers inside
def k32h(s):
    s = s.replace("void *minimumApplicationAddress;", "uint32_t minimumApplicationAddress;").replace("void *maximumApplicationAddress;", "uint32_t maximumApplicationAddress;")
    s = s.replace("void *reserved1, *reserved2, *reserved3;\n\t\tSDL_mutex *mutex;\n\t\tvoid *reserved4, *reserved5;", "uint32_t reserved1, reserved2, reserved3;\n\t\tuint32_t mutex;\n\t\tuint32_t reserved4, reserved5;")
    s = s.replace("\t\t\tvoid *Pointer;\n\t\t};\n\t\tvoid *hEvent;", "\t\t\tuint32_t Pointer;\n\t\t};\n\t\tuint32_t hEvent;")
    return s
rw(dst + "/Kernel32.h", k32h)
def k32c(s):
    s = s.replace("criticalSection->mutex = SDL_CreateMutex();", "criticalSection->mutex = (uint32_t)(uintptr_t)SDL_CreateMutex();")
    s = s.replace("SDL_LockMutex(criticalSection->mutex)", "SDL_LockMutex((SDL_mutex *)(uintptr_t)criticalSection->mutex)")
    s = s.replace("SDL_UnlockMutex(criticalSection->mutex)", "SDL_UnlockMutex((SDL_mutex *)(uintptr_t)criticalSection->mutex)")
    s = s.replace("SDL_DestroyMutex(criticalSection->mutex)", "SDL_DestroyMutex((SDL_mutex *)(uintptr_t)criticalSection->mutex)")
    s = s.replace("SetEvent_wrap(file->readOverlapped->hEvent)", "SetEvent_wrap((Event *)(uintptr_t)file->readOverlapped->hEvent)")
    s = s.replace("((Event *)overlapped->hEvent)", "((Event *)(uintptr_t)overlapped->hEvent)")
    s = s.replace("SetEvent_wrap(overlapped->hEvent)", "SetEvent_wrap((Event *)(uintptr_t)overlapped->hEvent)")
    return s
rw(dst + "/Kernel32.c", k32c)
rw(dst + "/User32.h", lambda s: s.replace("\tvoid *hWnd;\n\tuint32_t uMsg;", "\tuint32_t hWnd;\n\tuint32_t uMsg;"))
rw(dst + "/FetchTrackRecords.c", lambda s: s.replace("\tchar buffer[MAX_PATH];", "\tstatic char buffer[MAX_PATH]; /* passed to guest code: must be a low address */", 1))
# main/audio Game contexts are passed to wrappers as 32-bit values: allocate them low (lowmem malloc), not on the host stack
rw(dst + "/Cpp/NFS2SE.cpp", lambda s: s.replace("\tGame game, audio;\n", "\tGame &game = *new (malloc(sizeof(Game))) Game();\n\tGame &audio = *new (malloc(sizeof(Game))) Game();\n"))
def k32w(s):
    s = s.replace("Event *const *events, BOOL waitAll", "const uint32_t *events_, BOOL waitAll")
    s = s.replace("\tuint32_t i, ret = WAIT_TIMEOUT;\n\tSDL_LockMutex(event_mutex);", "\tuint32_t i, ret = WAIT_TIMEOUT;\n\tEvent *events[64]; for (i = 0; i < count && i < 64; ++i) events[i] = (Event *)(uintptr_t)events_[i];\n\tSDL_LockMutex(event_mutex);")
    s = s.replace("void *hTargetProcessHandle, void **lpTargetHandle", "void *hTargetProcessHandle, uint32_t *lpTargetHandle")
    return s
rw(dst + "/Kernel32.c", k32w)
rw(dst + "/DInput.h", lambda s: s.replace("\tuint32_t *rgdwAxes;\n\tuint32_t *rglDirection;\n\tDIENVELOPE *envelope;\n\tuint32_t cbTypeSpecificParams;\n\tvoid *typeSpecificParams;\n} DIEFFECT;",
    "\tuint32_t rgdwAxes;\n\tuint32_t rglDirection;\n\tuint32_t envelope;\n\tuint32_t cbTypeSpecificParams;\n\tuint32_t typeSpecificParams;\n} DIEFFECT;"))
rw(dst + "/DInput.c", lambda s: s.replace("di_eff->typeSpecificParams;", "(void *)(uintptr_t)di_eff->typeSpecificParams;")
                                 .replace("di_eff->rglDirection[0]", "((uint32_t *)(uintptr_t)di_eff->rglDirection)[0]"))
# guest data tables hold 4-byte pointers: members declared "void *" must be 32-bit slots
def datafix(s):
    out = []
    for ln in s.split("\n"):
        if ln.startswith("\tstruct packed {") and "void *" in ln:
            ln = ln.replace("void *", "uint32_t ")
        elif re.search(r"/\* \w+ \*/$", ln) and "(void *)" in ln:
            ln = ln.replace("(void *)", "(uint32_t)(uintptr_t)")
            ln = re.sub(r"\b(NULL|nullptr)\b", "0", ln)
        out.append(ln)
    return "\n".join(out)
rw(dst + "/Cpp/NFS2SE.cpp", datafix)
# no OpenGL window (Vulkan renders offscreen; SDL's dummy video driver on the console has no GL)
rw(dst + "/Wrapper.c", lambda s: s.replace("int windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI", "int windowFlags = SDL_WINDOW_ALLOW_HIGHDPI"))
# Glide: the OpenGL implementation is replaced by ps5/glide/vkglide.cpp (Vulkan)
os.remove(dst + "/Glide2x.c")
print("patched ->", dst)
