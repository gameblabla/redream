#
# Makefile for Linux
# It's mostly for conveniance and also so i don't have to recompile SDL2 each time.
#

CC = gcc
CXX = g++

X86_64_JIT = YES

OUTPUTNAME = redream.elf

DEFINES = -D_7ZIP_ST -DFLAC__HAS_OGG=0 -DPLATFORM_LINUX -DPACKAGE_VERSION=\"\"
DEFINES += -DHAVE_STRINGS_H -DHAVE_STRINGS_H -DHAVE_IMGUI -DHAVE_LROUND -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRCASECMP=0

INCLUDES = -I. -Ideps/capstone/include -Ideps/xbyak -Ideps/chdr/src -Ideps/inih 
INCLUDES += -Ideps/cimgui/cimgui -I./src -I/usr/include/SDL2 -I/usr/include/lzma -I./deps/chdr/deps/lzma-16.04/C -I./deps/chdr/deps/crypto
INCLUDES += -Ideps/cimgui -Ideps/chdr/deps/flac-1.3.2/include -Ideps/chdr/deps/flac-1.3.2/src/libFLAC/include
INCLUDES += -I./src/file -I./src/guest -I./src/jit -I./src/render -I./src/host -I./src/jit/backend/x64 
INCLUDES += -I./src/jit/backend/interp -I./src/jit/ir -I./src/jit/passes  -I./src/jit/frontend/armv3 
INCLUDES += -I./src/jit/frontend/sh4 -I./src/guest/aica -I./src/guest/arm7 -I./src/guest/bios -I./src/guest/gdrom 
INCLUDES += -I./src/guest/holly -I./src/guest/maple -I./src/guest/pvr -I./src/guest/rom -I./src/guest/serial -I./src/guest/sh4 -Ideps/glad/include

OPT_FLAGS  = -Ofast -march=native -mtune=native
# This is mandatory, otherwise it won't work properly
OPT_FLAGS  += -fms-extensions -D_GNU_SOURCE -fPIC
OPT_FLAGS  += -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-strict-aliasing

CFLAGS = $(DEFINES) $(INCLUDES) $(OPT_FLAGS) -std=gnu11 
CXXFLAGS = $(DEFINES) $(INCLUDES) $(OPT_FLAGS) -std=gnu++11 
LDFLAGS = -lrt -lSDL2 -lGL -lm -pthread -ldl -lstdc++

# Redream (main engine)
OBJS =  \
  src/core/exception_handler_linux.o \
  src/core/filesystem_posix.o \
  src/core/memory_posix.o \
  src/core/thread_posix.o \
  src/core/time_linux.o \
  src/core/assert.o \
  src/core/bitmap.o \
  src/core/exception_handler.o \
  src/core/filesystem.o \
  src/core/interval_tree.o \
  src/core/list.o \
  src/core/log.o \
  src/core/md5.o \
  src/core/memory.o \
  src/core/option.o \
  src/core/profiler.o \
  src/core/ringbuf.o \
  src/core/rb_tree.o \
  src/core/sort.o \
  src/core/string.o \
  src/file/trace.o \
  src/guest/aica/aica.o \
  src/guest/arm7/arm7.o \
  src/guest/bios/bios.o \
  src/guest/bios/flash.o \
  src/guest/bios/scramble.o \
  src/guest/bios/syscalls.o \
  src/guest/gdrom/cdi.o \
  src/guest/gdrom/chd.o \
  src/guest/gdrom/disc.o \
  src/guest/gdrom/gdi.o \
  src/guest/gdrom/gdrom.o \
  src/guest/holly/holly.o \
  src/guest/maple/controller.o \
  src/guest/maple/maple.o \
  src/guest/maple/vmu.o \
  src/guest/pvr/pvr.o \
  src/guest/pvr/ta.o \
  src/guest/pvr/tex.o \
  src/guest/pvr/tr.o \
  src/guest/rom/boot.o \
  src/guest/rom/flash.o \
  src/guest/serial/serial.o \
  src/guest/sh4/sh4.o \
  src/guest/sh4/sh4_ccn.o \
  src/guest/sh4/sh4_dbg.o \
  src/guest/sh4/sh4_dmac.o \
  src/guest/sh4/sh4_intc.o \
  src/guest/sh4/sh4_mem.o \
  src/guest/sh4/sh4_mmu.o \
  src/guest/sh4/sh4_tmu.o \
  src/guest/sh4/sh4_scif.o \
  src/guest/debugger.o \
  src/guest/dreamcast.o \
  src/guest/memory.o \
  src/guest/scheduler.o \
  src/host/keycode.o \
  src/jit/backend/interp/interp_backend.o \
  src/jit/frontend/armv3/armv3_context.o \
  src/jit/frontend/armv3/armv3_disasm.o \
  src/jit/frontend/armv3/armv3_fallback.o \
  src/jit/frontend/armv3/armv3_frontend.o \
  src/jit/frontend/sh4/sh4_disasm.o \
  src/jit/frontend/sh4/sh4_fallback.o \
  src/jit/frontend/sh4/sh4_frontend.o \
  src/jit/frontend/sh4/sh4_translate.o \
  src/jit/ir/ir.o \
  src/jit/ir/ir_read.o \
  src/jit/ir/ir_write.o \
  src/jit/passes/constant_propagation_pass.o \
  src/jit/passes/control_flow_analysis_pass.o \
  src/jit/passes/dead_code_elimination_pass.o \
  src/jit/passes/expression_simplification_pass.o \
  src/jit/passes/load_store_elimination_pass.o \
  src/jit/passes/register_allocation_pass.o \
  src/jit/passes/conversion_elimination_pass.o \
  src/jit/jit.o \
  src/jit/pass_stats.o \
  src/render/gl_backend.o \
  src/options.o \
  src/stats.o \
  
# UI and SDL
OBJS +=  \
	src/imgui.o \
	src/tracer.o \
	src/host/sdl_host.o \
	src/emulator.o \
	src/ui.o
  
# Glad
OBJS += \
	deps/glad/src/glad.o \
	
# chdr	
OBJS += \
	deps/chdr/src/bitstream.o \
	deps/chdr/src/cdrom.o \
	deps/chdr/src/chd.o \
	deps/chdr/src/flac.o \
	deps/chdr/src/huffman.o

# LibIni	
OBJS += \
	deps/inih/ini.o \
	
# libFlac
OBJS += \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/bitmath.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/bitreader.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/cpu.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/crc.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/fixed.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/fixed_intrin_sse2.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/fixed_intrin_ssse3.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/float.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/format.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/lpc.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/lpc_intrin_avx2.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/lpc_intrin_sse2.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/lpc_intrin_sse41.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/lpc_intrin_sse.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/md5.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/memory.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/metadata_iterators.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/metadata_object.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/stream_decoder.o \
	deps/chdr/deps/flac-1.3.2/src/libFLAC/window.o
	
# liblzma
OBJS += \
	deps/chdr/deps/lzma-16.04/C/Alloc.o \
	deps/chdr/deps/lzma-16.04/C/Bra86.o \
	deps/chdr/deps/lzma-16.04/C/Bra.o \
	deps/chdr/deps/lzma-16.04/C/BraIA64.o \
	deps/chdr/deps/lzma-16.04/C/CpuArch.o \
	deps/chdr/deps/lzma-16.04/C/Delta.o \
	deps/chdr/deps/lzma-16.04/C/LzFind.o \
	deps/chdr/deps/lzma-16.04/C/Lzma86Dec.o \
	deps/chdr/deps/lzma-16.04/C/Lzma86Enc.o \
	deps/chdr/deps/lzma-16.04/C/LzmaDec.o \
	deps/chdr/deps/lzma-16.04/C/LzmaEnc.o \
	deps/chdr/deps/lzma-16.04/C/LzmaLib.o \
	deps/chdr/deps/lzma-16.04/C/Sort.o \
	deps/chdr/deps/crypto/md5.o \
	deps/chdr/deps/crypto/sha1.o \

# libzlib
OBJS += deps/chdr/deps/zlib-1.2.11/adler32.o \
    deps/chdr/deps/zlib-1.2.11/compress.o \
    deps/chdr/deps/zlib-1.2.11/crc32.o \
    deps/chdr/deps/zlib-1.2.11/deflate.o \
    deps/chdr/deps/zlib-1.2.11/gzclose.o \
    deps/chdr/deps/zlib-1.2.11/gzlib.o \
    deps/chdr/deps/zlib-1.2.11/gzread.o \
    deps/chdr/deps/zlib-1.2.11/gzwrite.o \
    deps/chdr/deps/zlib-1.2.11/inflate.o \
    deps/chdr/deps/zlib-1.2.11/infback.o \
    deps/chdr/deps/zlib-1.2.11/inftrees.o \
    deps/chdr/deps/zlib-1.2.11/inffast.o \
    deps/chdr/deps/zlib-1.2.11/trees.o \
    deps/chdr/deps/zlib-1.2.11/uncompr.o \
    deps/chdr/deps/zlib-1.2.11/zutil.o

# cimgui
OBJS += deps/cimgui/cimgui/cimgui.o \
	deps/cimgui/cimgui/drawList.o \
	deps/cimgui/cimgui/fontAtlas.o \
	deps/cimgui/cimgui/listClipper.o \
	deps/cimgui/imgui/imgui.o \
	deps/cimgui/imgui/imgui_draw.o \
	deps/cimgui/imgui/imgui_demo.o \

# X86_64 Jit engine
ifeq ($(X86_64_JIT), YES)
OBJS +=    \
	src/jit/backend/x64/x64_backend.o \
    src/jit/backend/x64/x64_disassembler.o \
    src/jit/backend/x64/x64_dispatch.o \
    src/jit/backend/x64/x64_emitters.o \
    deps/capstone/cs.o \
    deps/capstone/SStream.o \
    deps/capstone/MCInst.o \
    deps/capstone/MCInstrDesc.o \
    deps/capstone/MCRegisterInfo.o \
    deps/capstone/utils.o \
    deps/capstone/arch/X86/X86Disassembler.o \
	deps/capstone/arch/X86/X86DisassemblerDecoder.o \
	deps/capstone/arch/X86/X86IntelInstPrinter.o \
	deps/capstone/arch/X86/X86Mapping.o \
	deps/capstone/arch/X86/X86Module.o \
	deps/capstone/arch/X86/X86ATTInstPrinter.o
	DEFINES += -DARCH_X64 -DCAPSTONE_HAS_X86 -DCAPSTONE_USE_SYS_DYN_MEM
endif
 
.c.o:
	$(CC) $(CFLAGS) -c -o $@ $< 
	
.CC.o:
	$(CXX) $(CXXFLAGS) -c -o $@ $< 
	
all: executable

executable : $(OBJS)
	$(CC) -o $(OUTPUTNAME) $(OBJS) $(CFLAGS) $(LDFLAGS)

clean:
	rm $(OBJS) $(OUTPUTNAME)
