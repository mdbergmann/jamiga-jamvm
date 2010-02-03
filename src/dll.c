/*
 * Copyright (C) 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010
 * Robert Lougher <rob@jamvm.org.uk>.
 *
 * This file is part of JamVM.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "jam.h"

#ifndef NO_JNI
#include "hash.h"
#include "jni.h"
#include "natives.h"
#include "symbol.h"
#include "excep.h"
#include "jni-stubs.h"

/* Set by call to initialise -- if true, prints out
    results of dynamic method resolution */
static int verbose;

static FILE *sig_trace_fd;

extern int nativeExtraArg(MethodBlock *mb);
extern uintptr_t *callJNIMethod(void *env, Class *class, char *sig, int extra,
                                uintptr_t *ostack, unsigned char *native_func,
                                int args);
extern void *jni_env;
extern JavaVM jni_invoke_intf; 

#define HASHTABSZE 1<<4
static HashTable hash_table;
NativeMethod lookupLoadedDlls(MethodBlock *mb);
#endif

/* Trace library loading and method lookup */
#ifdef TRACEDLL
#define TRACE(fmt, ...) jam_printf(fmt, ## __VA_ARGS__)
#else
#define TRACE(fmt, ...)
#endif

char *mangleString(char *utf8) {
    int len = utf8Len(utf8);
    unsigned short *unicode = sysMalloc(len * 2);
    char *mangled, *mngldPtr;
    int i, mangledLen = 0;

    convertUtf8(utf8, unicode);

    /* Work out the length of the mangled string */

    for(i = 0; i < len; i++) {
        unsigned short c = unicode[i];
        switch(c) {
            case '_':
            case ';':
            case '[':
                mangledLen += 2;
                break;

           default:
                mangledLen += isalnum(c) ? 1 : 6;
                break;
        }
    }

    mangled = mngldPtr = sysMalloc(mangledLen + 1);

    /* Construct the mangled string */

    for(i = 0; i < len; i++) {
        unsigned short c = unicode[i];
        switch(c) {
            case '_':
                *mngldPtr++ = '_';
                *mngldPtr++ = '1';
                break;
            case ';':
                *mngldPtr++ = '_';
                *mngldPtr++ = '2';
                break;
            case '[':
                *mngldPtr++ = '_';
                *mngldPtr++ = '3';
                break;

            case '/':
                *mngldPtr++ = '_';
                break;

            default:
                if(isalnum(c))
                    *mngldPtr++ = c;
                else
                    mngldPtr += sprintf(mngldPtr, "_0%04x", c);
                break;
        }
    }

    *mngldPtr = '\0';

    sysFree(unicode);
    return mangled;
}

char *mangleClassAndMethodName(MethodBlock *mb) {
    char *classname = CLASS_CB(mb->class)->name;
    char *methodname = mb->name;
    char *nonMangled = sysMalloc(strlen(classname) + strlen(methodname) + 7);
    char *mangled;

    sprintf(nonMangled, "Java/%s/%s", classname, methodname);

    mangled = mangleString(nonMangled);
    sysFree(nonMangled);
    return mangled;
}

char *mangleSignature(MethodBlock *mb) {
    char *type = mb->type;
    char *nonMangled;
    char *mangled;
    int i;

    /* find ending ) */
    for(i = strlen(type) - 1; type[i] != ')'; i--);

    nonMangled = sysMalloc(i);
    strncpy(nonMangled, type + 1, i - 1);
    nonMangled[i - 1] = '\0';
    
    mangled = mangleString(nonMangled);
    sysFree(nonMangled);
    return mangled;
}

NativeMethod lookupInternal(MethodBlock *mb) {
    ClassBlock *cb = CLASS_CB(mb->class);
    int i;

    TRACE("<DLL: Looking up %s internally>\n", mb->name);

    /* First try to locate the class */
    for(i = 0; native_methods[i].classname &&
        (strcmp(cb->name, native_methods[i].classname) != 0); i++);

    if(native_methods[i].classname) {
        VMMethod *methods = native_methods[i].methods;

        /* Found the class -- now try to locate the method */
        for(i = 0; methods[i].methodname &&
            (strcmp(mb->name, methods[i].methodname) != 0); i++);

        if(methods[i].methodname) {
            if(verbose)
                jam_printf("internal");

            /* Found it -- set the invoker to the native method */
            return mb->native_invoker = methods[i].method;
        }
    }

    return NULL;
}

NativeMethod resolveNativeMethod(MethodBlock *mb) {
    NativeMethod method;

    if(verbose) {
        char *classname = slash2dots(CLASS_CB(mb->class)->name);
        jam_printf("[Dynamic-linking native method %s.%s ... ",
                   classname, mb->name);
        sysFree(classname);
    }

    /* First see if it's an internal native method */
    method = lookupInternal(mb);

#ifndef NO_JNI
    if(method == NULL)
        method = lookupLoadedDlls(mb);
#endif

    if(verbose)
        jam_printf("]\n");

    return method;
}

uintptr_t *resolveNativeWrapper(Class *class, MethodBlock *mb,
                                uintptr_t *ostack) {

    NativeMethod method = resolveNativeMethod(mb);

    if(method == NULL) {
        signalException(java_lang_UnsatisfiedLinkError, mb->name);
        return ostack;
    }

    return (*method)(class, mb, ostack);
}

void initialiseDll(InitArgs *args) {
#ifndef NO_JNI
    /* Init hash table, and create lock */
    initHashTable(hash_table, HASHTABSZE, TRUE);

    if(args->trace_jni_sigs) {
        sig_trace_fd = fopen("jni-signatures", "w");
        if(sig_trace_fd == NULL) {
            perror("Couldn't open signatures file for writing");
            exit(1);
        }
    }
#endif
    verbose = args->verbosedll;
}

void shutdownDll() {
    if(sig_trace_fd != NULL)
        fclose(sig_trace_fd);
}

#ifndef NO_JNI
typedef struct {
    char *name;
    void *handle;
    Object *loader;
} DllEntry;

int dllNameHash(char *name) {
    int hash = 0;

    while(*name)
        hash = hash * 37 + *name++;

    return hash;
}

int resolveDll(char *name, Object *loader) {
    DllEntry *dll;

    TRACE("<DLL: Attempting to resolve library %s>\n", name);

#define HASH(ptr) dllNameHash(ptr)
#define COMPARE(ptr1, ptr2, hash1, hash2) \
                  ((hash1 == hash2) && (strcmp(ptr1, ptr2->name) == 0))
#define PREPARE(ptr) ptr
#define SCAVENGE(ptr) FALSE
#define FOUND(ptr1, ptr2) ptr2

    /* Do not add if absent, no scavenge, locked */
    findHashEntry(hash_table, name, dll, FALSE, FALSE, TRUE);

    if(dll == NULL) {
        DllEntry *dll2;
        void *onload, *handle = nativeLibOpen(name);

        if(handle == NULL) {
            if(verbose) {
                char *error = nativeLibError();

                jam_printf("[Failed to open library %s: %s]\n", name,
                           error == NULL ? "<no reason available>" : error);
            }
            return FALSE;
        }

        if((onload = nativeLibSym(handle, "JNI_OnLoad")) != NULL) {
            int ver;

            initJNILrefs();
            ver = (*(jint (*)(JavaVM*, void*))onload)(&jni_invoke_intf, NULL);

            if(ver != JNI_VERSION_1_2 && ver != JNI_VERSION_1_4
                                      && ver != JNI_VERSION_1_6) {
                if(verbose)
                    jam_printf("[%s: JNI_OnLoad returned unsupported version"
                               " number %d.\n>", name, ver);

                return FALSE;
            }
        }

        if(verbose)
           jam_printf("[Opened native library %s]\n", name);

        dll = sysMalloc(sizeof(DllEntry));
        dll->name = strcpy(sysMalloc(strlen(name) + 1), name);
        dll->handle = handle;
        dll->loader = loader;

#undef HASH
#undef COMPARE
#define HASH(ptr) dllNameHash(ptr->name)
#define COMPARE(ptr1, ptr2, hash1, hash2) \
                  ((hash1 == hash2) && (strcmp(ptr1->name, ptr2->name) == 0))

        /* Add if absent, no scavenge, locked */
        findHashEntry(hash_table, dll, dll2, TRUE, FALSE, TRUE);

        /* If the library has an OnUnload function it must be
           called from a running Java thread (i.e. not within
           the GC!). Create an unloader object which will be
           finalised when the class loader is collected.
           Note, only do this when there is a classloader -
           the bootstrap classloader will never be collected,
           therefore libraries loaded by it will never be
           unloaded */
        if(loader != NULL && nativeLibSym(dll->handle, "JNI_OnUnload") != NULL)
            newLibraryUnloader(loader, dll);

    } else
        if(dll->loader != loader) {
            if(verbose)
                jam_printf("[%s: already loaded by another classloader]\n");
            return FALSE;
        }

    return TRUE;
}

char *getDllPath() {
    char *env = nativeLibPath();
    return env ? env : "";
}

char *getBootDllPath() {
    return CLASSPATH_INSTALL_DIR"/lib/classpath";
}

char *getDllName(char *name) {
   return nativeLibMapName(name);
}

void *lookupLoadedDlls0(char *name, Object *loader) {
    TRACE("<DLL: Looking up %s loader %p in loaded DLL's>\n", name, loader);

#define ITERATE(ptr)                                          \
{                                                             \
    DllEntry *dll = (DllEntry*)ptr;                           \
    if(dll->loader == loader) {                               \
        void *sym = nativeLibSym(dll->handle, name);          \
        if(sym != NULL)                                       \
            return sym;                                       \
    }                                                         \
}

    hashIterate(hash_table);
    return NULL;
}

void unloadDll(DllEntry *dll, int unloader) {
    void *on_unload = nativeLibSym(dll->handle, "JNI_OnUnload");

    if(unloader || on_unload == NULL) {
        TRACE("<DLL: Unloading DLL %s\n", dll->name);

        if(on_unload != NULL) {
            initJNILrefs();
            (*(void (*)(JavaVM*, void*))on_unload)(&jni_invoke_intf, NULL);
        }

        nativeLibClose(dll->handle);
        sysFree(dll->name);
        sysFree(dll);
    }
}

void unloaderUnloadDll(uintptr_t entry) {
    unloadDll((DllEntry*)entry, TRUE);
}

#undef ITERATE
#define ITERATE(ptr)                                          \
{                                                             \
    DllEntry *dll = (DllEntry*)ptr;                           \
    if(isMarked(dll->loader))                                 \
        threadReference(&dll->loader);                        \
}

void threadLiveClassLoaderDlls() {
    hashIterate(hash_table);
}

void unloadClassLoaderDlls(Object *loader) {
    int unloaded = 0;

    TRACE("<DLL: Unloading DLLs for loader %p\n", loader);

#undef ITERATE
#define ITERATE(ptr)                                          \
{                                                             \
    DllEntry *dll = (DllEntry*)*ptr;                          \
    if(dll->loader == loader) {                               \
        unloadDll(dll, FALSE);                                \
        *ptr = NULL;                                          \
        unloaded++;                                           \
    }                                                         \
}

    hashIterateP(hash_table);

    if(unloaded) {
        int size;

        /* Update count to remaining number of DLLs */
        hash_table.hash_count -= unloaded;

        /* Calculate nearest multiple of 2 larger than count */
        for(size = 1; size < hash_table.hash_count; size <<= 1);

        /* Ensure new table is less than 2/3 full */
        size = hash_table.hash_count*3 > size*2 ? size<< 1 : size;

        resizeHash(&hash_table, size);
    }
}

uintptr_t *callJNIWrapper(Class *class, MethodBlock *mb, uintptr_t *ostack) {
    TRACE("<DLL: Calling JNI method %s.%s%s>\n", CLASS_CB(class)->name,
          mb->name, mb->type);

    if(!initJNILrefs())
        return NULL;

    return callJNIMethod(&jni_env,
                         (mb->access_flags & ACC_STATIC) ? class : NULL,
                         mb->simple_sig, mb->native_extra_arg, ostack,
                         mb->code, mb->args_count);
}

NativeMethod findJNIStub(char *sig, JNIStub *stubs) {
    int i;

    for(i = 0; stubs[i].signature != NULL &&
               strcmp(sig, stubs[i].signature) != 0; i++);

    if(stubs[i].signature == NULL)
        return NULL;

    return stubs[i].func;
}

NativeMethod setJNIMethod(MethodBlock *mb, void *func) {
    char *simple = convertSig2Simple(mb->type);
    NativeMethod invoker;

    if(mb->access_flags & ACC_STATIC)
        invoker = findJNIStub(simple, jni_static_stubs);
    else
        invoker = findJNIStub(simple, jni_stubs);

    if(invoker == NULL) {
        if(sig_trace_fd != NULL)
            fprintf(sig_trace_fd, "%s%s\n", mb->access_flags & ACC_STATIC ?
                                            "static " : "", mb->type);

        mb->native_extra_arg = nativeExtraArg(mb);
        invoker = &callJNIWrapper;

        if((mb->simple_sig = newUtf8(simple)) != simple)
            sysFree(simple);
    } else
        sysFree(simple);

    mb->code = func;
    return mb->native_invoker = invoker;
}

NativeMethod lookupLoadedDlls(MethodBlock *mb) {
    Object *loader = (CLASS_CB(mb->class))->class_loader;
    char *mangled = mangleClassAndMethodName(mb);
    void *func = lookupLoadedDlls0(mangled, loader);

    if(func == NULL) {
        char *mangledSig = mangleSignature(mb);
        char *fullyMangled = sysMalloc(strlen(mangled)+strlen(mangledSig)+3);

        sprintf(fullyMangled, "%s__%s", mangled, mangledSig);
        func = lookupLoadedDlls0(fullyMangled, loader);

        sysFree(fullyMangled);
        sysFree(mangledSig);
    }

    sysFree(mangled);

    if(func) {
        if(verbose)
            jam_printf("JNI");

        return setJNIMethod(mb, func);
    }

    return NULL;
}
#endif

