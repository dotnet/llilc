//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
//
// The code in sos.DumpLog depends on the first 32 facility codes 
// being bit flags sorted in incresing order.

DEFINE_LOG_FACILITY(LF_GC           ,0x00000001)
DEFINE_LOG_FACILITY(LF_GCINFO       ,0x00000002)
DEFINE_LOG_FACILITY(LF_STUBS        ,0x00000004)
DEFINE_LOG_FACILITY(LF_JIT          ,0x00000008)
DEFINE_LOG_FACILITY(LF_LOADER       ,0x00000010)
DEFINE_LOG_FACILITY(LF_METADATA     ,0x00000020)
DEFINE_LOG_FACILITY(LF_SYNC         ,0x00000040)
DEFINE_LOG_FACILITY(LF_EEMEM        ,0x00000080)
DEFINE_LOG_FACILITY(LF_GCALLOC      ,0x00000100)
DEFINE_LOG_FACILITY(LF_CORDB        ,0x00000200)
DEFINE_LOG_FACILITY(LF_CLASSLOADER  ,0x00000400)
DEFINE_LOG_FACILITY(LF_CORPROF      ,0x00000800)
DEFINE_LOG_FACILITY(LF_REMOTING     ,0x00001000)
DEFINE_LOG_FACILITY(LF_DBGALLOC     ,0x00002000)
DEFINE_LOG_FACILITY(LF_EH           ,0x00004000)
DEFINE_LOG_FACILITY(LF_ENC          ,0x00008000)
DEFINE_LOG_FACILITY(LF_ASSERT       ,0x00010000)
DEFINE_LOG_FACILITY(LF_VERIFIER     ,0x00020000)
DEFINE_LOG_FACILITY(LF_THREADPOOL   ,0x00040000)
DEFINE_LOG_FACILITY(LF_GCROOTS      ,0x00080000)
DEFINE_LOG_FACILITY(LF_INTEROP      ,0x00100000)
DEFINE_LOG_FACILITY(LF_MARSHALER    ,0x00200000)
DEFINE_LOG_FACILITY(LF_IJW          ,0x00400000)
DEFINE_LOG_FACILITY(LF_ZAP          ,0x00800000)
DEFINE_LOG_FACILITY(LF_STARTUP      ,0x01000000)  // Log startupa and shutdown failures
DEFINE_LOG_FACILITY(LF_APPDOMAIN    ,0x02000000)
DEFINE_LOG_FACILITY(LF_CODESHARING  ,0x04000000)
DEFINE_LOG_FACILITY(LF_STORE        ,0x08000000)
DEFINE_LOG_FACILITY(LF_SECURITY     ,0x10000000)
DEFINE_LOG_FACILITY(LF_LOCKS        ,0x20000000)
DEFINE_LOG_FACILITY(LF_BCL          ,0x40000000)
//                  LF_ALWAYS        0x80000000     // make certain you don't try to use this bit for a real facility
//                  LF_ALL           0xFFFFFFFF
//
#undef DEFINE_LOG_FACILITY

