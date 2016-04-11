#include <imgui.h>
#include "core/math.h"
#include "core/memory.h"
#include "core/profiler.h"
#include "hw/sh4/sh4.h"
#include "hw/dreamcast.h"
#include "hw/debugger.h"
#include "hw/memory.h"
#include "hw/scheduler.h"

using namespace re;
using namespace re::hw;
using namespace re::hw::sh4;
using namespace re::jit;
using namespace re::jit::backend;
using namespace re::jit::frontend::sh4;
using namespace re::sys;

static InterruptInfo interrupts[NUM_INTERRUPTS] = {
#define SH4_INT(name, intevt, pri, ipr, ipr_shift) \
  { intevt, pri, ipr, ipr_shift }                  \
  ,
#include "hw/sh4/sh4_int.inc"
#undef SH4_INT
};

static SH4 *s_current_cpu = nullptr;

enum {
  SH4_CLOCK_FREQ = 200000000,
};

SH4::SH4(Dreamcast &dc)
    : Device(dc),
      DebugInterface(this),
      ExecuteInterface(this),
      MemoryInterface(this),
      WindowInterface(this),
      dc_(dc),
      memory_(nullptr),
      scheduler_(nullptr),
      code_cache_(nullptr),
      regs_(),
      cache_(),
      show_perf_(false),
      mips_(),
      num_mips_(0),
      requested_interrupts_(0),
      pending_interrupts_(0),
      tmu_timers_{INVALID_TIMER, INVALID_TIMER, INVALID_TIMER},
      tmu_delegates_{re::make_delegate(&SH4::ExpireTimer<0>, this),
                     re::make_delegate(&SH4::ExpireTimer<1>, this),
                     re::make_delegate(&SH4::ExpireTimer<2>, this)} {}

SH4::~SH4() { delete code_cache_; }

bool SH4::Init() {
  memory_ = dc_.memory;
  scheduler_ = dc_.scheduler;

  code_cache_ =
      new SH4CodeCache({&ctx_, memory_->protected_base(), memory_, &Memory::R8,
                        &Memory::R16, &Memory::R32, &Memory::R64, &Memory::W8,
                        &Memory::W16, &Memory::W32, &Memory::W64},
                       &SH4::CompilePC);

  // initialize context
  memset(&ctx_, 0, sizeof(ctx_));
  ctx_.sh4 = this;
  ctx_.InvalidInstruction = &SH4::InvalidInstruction;
  ctx_.Prefetch = &SH4::Prefetch;
  ctx_.SRUpdated = &SH4::SRUpdated;
  ctx_.FPSCRUpdated = &SH4::FPSCRUpdated;
  ctx_.pc = 0xa0000000;
  ctx_.pr = 0x0;
  ctx_.sr = 0x700000f0;
  ctx_.fpscr = 0x00040001;

  // clear cache
  memset(cache_, 0, sizeof(cache_));

// initialize registers
#define SH4_REG(addr, name, flags, default, reset, sleep, standby, type) \
  if (default != HELD) {                                                 \
    regs_[name##_OFFSET] = {flags, default};                             \
  }
#include "hw/sh4/sh4_regs.inc"
#undef SH4_REG

  SH4_REGISTER_R32_DELEGATE(PDTRA);
  SH4_REGISTER_W32_DELEGATE(MMUCR);
  SH4_REGISTER_W32_DELEGATE(CCR);
  SH4_REGISTER_W32_DELEGATE(CHCR0);
  SH4_REGISTER_W32_DELEGATE(CHCR1);
  SH4_REGISTER_W32_DELEGATE(CHCR2);
  SH4_REGISTER_W32_DELEGATE(CHCR3);
  SH4_REGISTER_W32_DELEGATE(DMAOR);
  SH4_REGISTER_W32_DELEGATE(IPRA);
  SH4_REGISTER_W32_DELEGATE(IPRB);
  SH4_REGISTER_W32_DELEGATE(IPRC);
  SH4_REGISTER_W32_DELEGATE(TSTR);
  SH4_REGISTER_W32_DELEGATE(TCR0);
  SH4_REGISTER_W32_DELEGATE(TCR1);
  SH4_REGISTER_W32_DELEGATE(TCR2);
  SH4_REGISTER_R32_DELEGATE(TCNT0);
  SH4_REGISTER_W32_DELEGATE(TCNT0);
  SH4_REGISTER_R32_DELEGATE(TCNT1);
  SH4_REGISTER_W32_DELEGATE(TCNT1);
  SH4_REGISTER_R32_DELEGATE(TCNT2);
  SH4_REGISTER_W32_DELEGATE(TCNT2);

  // reset interrupts
  ReprioritizeInterrupts();

  return true;
}

void SH4::SetPC(uint32_t pc) { ctx_.pc = pc; }

void SH4::Run(const std::chrono::nanoseconds &delta) {
  PROFILER_RUNTIME("SH4::Execute");

  // execute at least 1 cycle. the tests rely on this to step block by block
  int64_t cycles = std::max(NANO_TO_CYCLES(delta, SH4_CLOCK_FREQ), INT64_C(1));

  // set current sh4 instance for CompilePC
  s_current_cpu = this;

  // each block's epilog will decrement the remaining cycles as they run
  ctx_.num_cycles = static_cast<int>(cycles);

  while (ctx_.num_cycles > 0) {
    SH4BlockEntry *block = code_cache_->GetBlock(ctx_.pc);
    ctx_.pc = block->run();

    CheckPendingInterrupts();
  }

  // track mips
  auto now = std::chrono::high_resolution_clock::now();
  auto next_time = last_mips_time_ + std::chrono::seconds(1);

  if (now > next_time) {
    auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - last_mips_time_);
    auto delta_f =
        std::chrono::duration_cast<std::chrono::duration<float>>(delta).count();
    auto delta_scaled = delta_f * 1000000.0f;
    mips_[num_mips_++ % MAX_MIPS_SAMPLES] = ctx_.num_instrs / delta_scaled;
    ctx_.num_instrs = 0;
    last_mips_time_ = now;
  }

  s_current_cpu = nullptr;
}

void SH4::DDT(const DTR &dtr) {
  if (dtr.data) {
    // single address mode transfer
    if (dtr.rw) {
      memory_->Memcpy(dtr.addr, dtr.data, dtr.size);
    } else {
      memory_->Memcpy(dtr.data, dtr.addr, dtr.size);
    }
  } else {
    // dual address mode transfer
    // NOTE this should be made asynchronous, at which point the significance
    // of the registers / interrupts should be more obvious
    uint32_t *sar;
    uint32_t *dar;
    uint32_t *dmatcr;
    CHCR_T *chcr;
    Interrupt dmte;

    switch (dtr.channel) {
      case 0:
        sar = &SAR0;
        dar = &DAR0;
        dmatcr = &DMATCR0;
        chcr = &CHCR0;
        dmte = SH4_INTC_DMTE0;
        break;
      case 1:
        sar = &SAR1;
        dar = &DAR1;
        dmatcr = &DMATCR1;
        chcr = &CHCR1;
        dmte = SH4_INTC_DMTE1;
        break;
      case 2:
        sar = &SAR2;
        dar = &DAR2;
        dmatcr = &DMATCR2;
        chcr = &CHCR2;
        dmte = SH4_INTC_DMTE2;
        break;
      case 3:
        sar = &SAR3;
        dar = &DAR3;
        dmatcr = &DMATCR3;
        chcr = &CHCR3;
        dmte = SH4_INTC_DMTE3;
        break;
      default:
        LOG_FATAL("Unexpected DMA channel");
        break;
    }

    uint32_t src = dtr.rw ? dtr.addr : *sar;
    uint32_t dst = dtr.rw ? *dar : dtr.addr;
    int size = *dmatcr * 32;
    memory_->Memcpy(dst, src, size);

    // update src / addresses as well as remaining count
    *sar = src + size;
    *dar = dst + size;
    *dmatcr = 0;

    // signal transfer end
    chcr->TE = 1;

    // raise interrupt if requested
    if (chcr->IE) {
      RequestInterrupt(dmte);
    }
  }
}

void SH4::RequestInterrupt(Interrupt intr) {
  requested_interrupts_ |= sort_id_[intr];
  UpdatePendingInterrupts();
}

void SH4::UnrequestInterrupt(Interrupt intr) {
  requested_interrupts_ &= ~sort_id_[intr];
  UpdatePendingInterrupts();
}

int SH4::NumRegisters() { return 59; }

void SH4::Step() {
  // invalidate the block for the current pc
  code_cache_->RemoveBlocks(ctx_.pc);

  // recompile it with only one instruction and run it
  uint32_t guest_addr = ctx_.pc;
  uint8_t *host_addr = memory_->TranslateVirtual(guest_addr);
  int flags = GetCompileFlags() | SH4_SINGLE_INSTR;

  SH4BlockEntry *block =
      code_cache_->CompileBlock(guest_addr, host_addr, flags);
  ctx_.pc = block->run();

  // let the debugger know we've stopped
  dc_.debugger->Trap();
}

void SH4::AddBreakpoint(int type, uint32_t addr) {
  // save off the original instruction
  uint16_t instr = memory_->R16(addr);
  breakpoints_.insert(std::make_pair(addr, instr));

  // write out an invalid instruction
  memory_->W16(addr, 0);

  code_cache_->RemoveBlocks(addr);
}

void SH4::RemoveBreakpoint(int type, uint32_t addr) {
  // recover the original instruction
  auto it = breakpoints_.find(addr);
  CHECK_NE(it, breakpoints_.end());
  uint16_t instr = it->second;
  breakpoints_.erase(it);

  // overwrite the invalid instruction with the original
  memory_->W16(addr, instr);

  code_cache_->RemoveBlocks(addr);
}

void SH4::ReadMemory(uint32_t addr, uint8_t *buffer, int size) {
  memory_->Memcpy(buffer, addr, size);
}

void SH4::ReadRegister(int n, uint64_t *value, int *size) {
  if (n < 16) {
    *value = ctx_.r[n];
  } else if (n == 16) {
    *value = ctx_.pc;
  } else if (n == 17) {
    *value = ctx_.pr;
  } else if (n == 18) {
    *value = ctx_.gbr;
  } else if (n == 19) {
    *value = ctx_.vbr;
  } else if (n == 20) {
    *value = ctx_.mach;
  } else if (n == 21) {
    *value = ctx_.macl;
  } else if (n == 22) {
    *value = ctx_.sr;
  } else if (n == 23) {
    *value = ctx_.fpul;
  } else if (n == 24) {
    *value = ctx_.fpscr;
  } else if (n < 41) {
    *value = ctx_.fr[n - 25];
  } else if (n == 41) {
    *value = ctx_.ssr;
  } else if (n == 42) {
    *value = ctx_.spc;
  } else if (n < 51) {
    uint32_t *b0 = (ctx_.sr & RB) ? ctx_.ralt : ctx_.r;
    *value = b0[n - 43];
  } else if (n < 59) {
    uint32_t *b1 = (ctx_.sr & RB) ? ctx_.r : ctx_.ralt;
    *value = b1[n - 51];
  }

  *size = 4;
}

void SH4::MapPhysicalMemory(Memory &memory, MemoryMap &memmap) {
  // area 2 and 4 are unused
  RegionHandle a0_handle = memory.AllocRegion(AREA0_BEGIN, AREA0_SIZE);
  RegionHandle a1_handle = memory.AllocRegion(AREA1_BEGIN, AREA1_SIZE);
  RegionHandle a3_handle = memory.AllocRegion(AREA3_BEGIN, AREA3_SIZE);
  RegionHandle a5_handle = memory.AllocRegion(AREA5_BEGIN, AREA5_SIZE);
  RegionHandle a6_handle = memory.AllocRegion(AREA6_BEGIN, AREA6_SIZE);
  RegionHandle a7_handle = memory.AllocRegion(AREA7_BEGIN, AREA7_SIZE);

  RegionHandle sh4_reg_handle = memory.AllocRegion(
      SH4_REG_BEGIN, SH4_REG_SIZE,
      make_delegate(&SH4::ReadRegister<uint8_t>, this),
      make_delegate(&SH4::ReadRegister<uint16_t>, this),
      make_delegate(&SH4::ReadRegister<uint32_t>, this), nullptr,
      make_delegate(&SH4::WriteRegister<uint8_t>, this),
      make_delegate(&SH4::WriteRegister<uint16_t>, this),
      make_delegate(&SH4::WriteRegister<uint32_t>, this), nullptr);

  memmap.Mount(a0_handle, AREA0_SIZE, AREA0_BEGIN);
  memmap.Mount(a1_handle, AREA1_SIZE, AREA1_BEGIN);
  memmap.Mount(a3_handle, AREA3_SIZE, AREA3_BEGIN);
  memmap.Mount(a5_handle, AREA5_SIZE, AREA5_BEGIN);
  memmap.Mount(a6_handle, AREA6_SIZE, AREA6_BEGIN);
  memmap.Mount(a7_handle, AREA7_SIZE, AREA7_BEGIN);
  memmap.Mount(sh4_reg_handle, SH4_REG_SIZE, SH4_REG_BEGIN);
}

void SH4::MapVirtualMemory(Memory &memory, MemoryMap &memmap) {
  RegionHandle sh4_cache_handle =
      memory.AllocRegion(SH4_CACHE_BEGIN, SH4_CACHE_SIZE,
                         make_delegate(&SH4::ReadCache<uint8_t>, this),
                         make_delegate(&SH4::ReadCache<uint16_t>, this),
                         make_delegate(&SH4::ReadCache<uint32_t>, this),
                         make_delegate(&SH4::ReadCache<uint64_t>, this),
                         make_delegate(&SH4::WriteCache<uint8_t>, this),
                         make_delegate(&SH4::WriteCache<uint16_t>, this),
                         make_delegate(&SH4::WriteCache<uint32_t>, this),
                         make_delegate(&SH4::WriteCache<uint64_t>, this));

  RegionHandle sh4_sq_handle = memory.AllocRegion(
      SH4_SQ_BEGIN, SH4_SQ_SIZE, make_delegate(&SH4::ReadSQ<uint8_t>, this),
      make_delegate(&SH4::ReadSQ<uint16_t>, this),
      make_delegate(&SH4::ReadSQ<uint32_t>, this), nullptr,
      make_delegate(&SH4::WriteSQ<uint8_t>, this),
      make_delegate(&SH4::WriteSQ<uint16_t>, this),
      make_delegate(&SH4::WriteSQ<uint32_t>, this), nullptr);

  // main ram mirrors
  memmap.Mirror(MAIN_RAM_1_BEGIN, MAIN_RAM_1_SIZE, MAIN_RAM_2_BEGIN);
  memmap.Mirror(MAIN_RAM_1_BEGIN, MAIN_RAM_1_SIZE, MAIN_RAM_3_BEGIN);
  memmap.Mirror(MAIN_RAM_1_BEGIN, MAIN_RAM_1_SIZE, MAIN_RAM_4_BEGIN);

  // physical mirrors (ignoring p, alt and cache bits in bits 31-29)
  memmap.Mirror(P0_1_BEGIN, P0_1_SIZE, P0_2_BEGIN);
  memmap.Mirror(P0_1_BEGIN, P0_1_SIZE, P0_3_BEGIN);
  memmap.Mirror(P0_1_BEGIN, P0_1_SIZE, P0_4_BEGIN);
  memmap.Mirror(P0_1_BEGIN, P0_1_SIZE, P1_BEGIN);
  memmap.Mirror(P0_1_BEGIN, P0_1_SIZE, P2_BEGIN);
  memmap.Mirror(P0_1_BEGIN, P0_1_SIZE, P3_BEGIN);
  memmap.Mirror(P0_1_BEGIN, P0_1_SIZE, P4_BEGIN);

  // handle some special access only available in P4 after applying mirrors
  memmap.Mount(sh4_cache_handle, SH4_CACHE_SIZE, SH4_CACHE_BEGIN);
  memmap.Mount(sh4_sq_handle, SH4_SQ_SIZE, SH4_SQ_BEGIN);
}

void SH4::OnPaint(bool show_main_menu) {
  if (show_main_menu && ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("CPU")) {
      ImGui::MenuItem("Perf", "", &show_perf_);
      ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
  }

  if (show_perf_) {
    ImGui::Begin("Perf", nullptr, ImGuiWindowFlags_NoTitleBar |
                                      ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::SetWindowPos(ImVec2(
        ImGui::GetIO().DisplaySize.x - ImGui::GetWindowSize().x - 10, 10));

    // calculate average mips
    float avg_mips = 0.0f;
    for (int i = std::max(0, num_mips_ - MAX_MIPS_SAMPLES); i < num_mips_;
         i++) {
      avg_mips += mips_[i % MAX_MIPS_SAMPLES];
    }
    avg_mips /= std::max(std::min(num_mips_, MAX_MIPS_SAMPLES), 1);

    char overlay_text[128];
    snprintf(overlay_text, sizeof(overlay_text), "%.2f", avg_mips);
    ImGui::PlotLines("MIPS", mips_, MAX_MIPS_SAMPLES, num_mips_, overlay_text,
                     0.0f, 400.0f);

    ImGui::End();
  }
}

uint32_t SH4::CompilePC() {
  uint32_t guest_addr = s_current_cpu->ctx_.pc;
  uint8_t *host_addr = s_current_cpu->memory_->TranslateVirtual(guest_addr);
  int flags = s_current_cpu->GetCompileFlags();

  SH4BlockEntry *block =
      s_current_cpu->code_cache_->CompileBlock(guest_addr, host_addr, flags);

  return block->run();
}

void SH4::InvalidInstruction(SH4Context *ctx, uint64_t data) {
  SH4 *self = reinterpret_cast<SH4 *>(ctx->sh4);
  uint32_t addr = static_cast<uint32_t>(data);

  auto it = self->breakpoints_.find(addr);
  CHECK_NE(it, self->breakpoints_.end());

  // force the main loop to break
  self->ctx_.num_cycles = 0;

  // let the debugger know execution has stopped
  self->dc_.debugger->Trap();
}

void SH4::Prefetch(SH4Context *ctx, uint64_t data) {
  SH4 *self = reinterpret_cast<SH4 *>(ctx->sh4);
  uint32_t addr = static_cast<uint32_t>(data);

  // only concerned about SQ related prefetches
  if (addr < 0xe0000000 || addr > 0xe3ffffff) {
    return;
  }

  // figure out the source and destination
  uint32_t dest = addr & 0x03ffffe0;
  uint32_t sqi = (addr & 0x20) >> 5;
  if (sqi) {
    dest |= (self->QACR1 & 0x1c) << 24;
  } else {
    dest |= (self->QACR0 & 0x1c) << 24;
  }

  // perform the "burst" 32-byte copy
  for (int i = 0; i < 8; i++) {
    self->memory_->W32(dest, ctx->sq[sqi][i]);
    dest += 4;
  }
}

void SH4::SRUpdated(SH4Context *ctx, uint64_t old_sr) {
  SH4 *self = reinterpret_cast<SH4 *>(ctx->sh4);

  if ((ctx->sr & RB) != (old_sr & RB)) {
    self->SwapRegisterBank();
  }

  if ((ctx->sr & I) != (old_sr & I) || (ctx->sr & BL) != (old_sr & BL)) {
    self->UpdatePendingInterrupts();
  }
}

void SH4::FPSCRUpdated(SH4Context *ctx, uint64_t old_fpscr) {
  SH4 *self = reinterpret_cast<SH4 *>(ctx->sh4);

  if ((ctx->fpscr & FR) != (old_fpscr & FR)) {
    self->SwapFPRegisterBank();
  }
}

int SH4::GetCompileFlags() {
  int flags = 0;
  if (ctx_.fpscr & PR) {
    flags |= SH4_DOUBLE_PR;
  }
  if (ctx_.fpscr & SZ) {
    flags |= SH4_DOUBLE_SZ;
  }
  return flags;
}

void SH4::SwapRegisterBank() {
  for (int s = 0; s < 8; s++) {
    uint32_t tmp = ctx_.r[s];
    ctx_.r[s] = ctx_.ralt[s];
    ctx_.ralt[s] = tmp;
  }
}

void SH4::SwapFPRegisterBank() {
  for (int s = 0; s <= 15; s++) {
    uint32_t tmp = ctx_.fr[s];
    ctx_.fr[s] = ctx_.xf[s];
    ctx_.xf[s] = tmp;
  }
}

template <typename T>
T SH4::ReadRegister(uint32_t addr) {
  uint32_t offset = SH4_REG_OFFSET(addr);
  Register &reg = regs_[offset];

  if (!(reg.flags & R)) {
    LOG_WARNING("Invalid read access at 0x%x", addr);
    return 0;
  }

  if (reg.read) {
    return reg.read(reg);
  }

  return static_cast<T>(reg.value);
}

template <typename T>
void SH4::WriteRegister(uint32_t addr, T value) {
  uint32_t offset = SH4_REG_OFFSET(addr);
  Register &reg = regs_[offset];

  if (!(reg.flags & W)) {
    LOG_WARNING("Invalid write access at 0x%x", addr);
    return;
  }

  uint32_t old_value = reg.value;
  reg.value = static_cast<uint32_t>(value);

  if (reg.write) {
    reg.write(reg, old_value);
  }
}

// with OIX, bit 25, rather than bit 13, determines which 4kb bank to use
#define CACHE_OFFSET(addr, OIX) \
  ((OIX ? ((addr & 0x2000000) >> 13) : ((addr & 0x2000) >> 1)) | (addr & 0xfff))

template <typename T>
T SH4::ReadCache(uint32_t addr) {
  CHECK_EQ(CCR.ORA, 1u);
  addr = CACHE_OFFSET(addr, CCR.OIX);
  return re::load<T>(&cache_[addr]);
}

template <typename T>
void SH4::WriteCache(uint32_t addr, T value) {
  CHECK_EQ(CCR.ORA, 1u);
  addr = CACHE_OFFSET(addr, CCR.OIX);
  re::store(&cache_[addr], value);
}

template <typename T>
T SH4::ReadSQ(uint32_t addr) {
  uint32_t sqi = (addr & 0x20) >> 5;
  unsigned idx = (addr & 0x1c) >> 2;
  return static_cast<T>(ctx_.sq[sqi][idx]);
}

template <typename T>
void SH4::WriteSQ(uint32_t addr, T value) {
  uint32_t sqi = (addr & 0x20) >> 5;
  uint32_t idx = (addr & 0x1c) >> 2;
  ctx_.sq[sqi][idx] = static_cast<uint32_t>(value);
}

//
// CCN
//

void SH4::ResetCache() {
  // FIXME this isn't right. When the IC is reset a pending flag is set and the
  // cache is actually reset at the end of the current block. However, the docs
  // for the SH4 IC state "After CCR is updated, an instruction that performs
  // data
  // access to the P0, P1, P3, or U0 area should be located at least four
  // instructions after the CCR update instruction. Also, a branch instruction
  // to
  // the P0, P1, P3, or U0 area should be located at least eight instructions
  // after the CCR update instruction."
  LOG_INFO("Reset instruction cache");

  code_cache_->UnlinkBlocks();
}

//
// DMAC
//
void SH4::CheckDMA(int channel) {
  CHCR_T *chcr = nullptr;

  switch (channel) {
    case 0:
      chcr = &CHCR0;
      break;
    case 1:
      chcr = &CHCR1;
      break;
    case 2:
      chcr = &CHCR2;
      break;
    case 3:
      chcr = &CHCR3;
      break;
    default:
      LOG_FATAL("Unexpected DMA channel");
      break;
  }

  CHECK(DMAOR.DDT || !DMAOR.DME || !chcr->DE, "Non-DDT DMA not supported");
}

//
// INTC
//

// Generate a sorted set of interrupts based on their priority. These sorted
// ids are used to represent all of the currently requested interrupts as a
// simple bitmask.
void SH4::ReprioritizeInterrupts() {
  uint64_t old = requested_interrupts_;
  requested_interrupts_ = 0;

  for (int i = 0, n = 0; i < 16; i++) {
    // for even priorities, give precedence to lower id interrupts
    for (int j = NUM_INTERRUPTS - 1; j >= 0; j--) {
      InterruptInfo &int_info = interrupts[j];

      // get current priority for interrupt
      int priority = int_info.default_priority;
      if (int_info.ipr) {
        Register &ipr_reg = regs_[int_info.ipr];
        priority = ((ipr_reg.value & 0xffff) >> int_info.ipr_shift) & 0xf;
      }

      if (priority != i) {
        continue;
      }

      bool was_requested = old & sort_id_[j];

      sorted_interrupts_[n] = (Interrupt)j;
      sort_id_[j] = (uint64_t)1 << n;
      n++;

      if (was_requested) {
        // rerequest with new sorted id
        requested_interrupts_ |= sort_id_[j];
      }
    }

    // generate a mask for all interrupts up to the current priority
    priority_mask_[i] = ((uint64_t)1 << n) - 1;
  }

  UpdatePendingInterrupts();
}

void SH4::UpdatePendingInterrupts() {
  int min_priority = (ctx_.sr & I) >> 4;
  uint64_t priority_mask = (ctx_.sr & BL) ? 0 : ~priority_mask_[min_priority];
  pending_interrupts_ = requested_interrupts_ & priority_mask;
}

inline void SH4::CheckPendingInterrupts() {
  if (!pending_interrupts_) {
    return;
  }

  // process the highest priority in the pending vector
  int n = 63 - re::clz(pending_interrupts_);
  Interrupt intr = sorted_interrupts_[n];
  InterruptInfo &int_info = interrupts[intr];

  INTEVT = int_info.intevt;
  ctx_.ssr = ctx_.sr;
  ctx_.spc = ctx_.pc;
  ctx_.sgr = ctx_.r[15];
  ctx_.sr |= (BL | MD | RB);
  ctx_.pc = ctx_.vbr + 0x600;

  SRUpdated(&ctx_, ctx_.ssr);
}

//
// TMU
//
static const int64_t PERIPHERAL_CLOCK_FREQ = SH4_CLOCK_FREQ >> 2;
static const int PERIPHERAL_SCALE[] = {2, 4, 6, 8, 10, 0, 0, 0};

#define TSTR(n) (TSTR & (1 << n))
#define TCOR(n) (n == 0 ? TCOR0 : n == 1 ? TCOR1 : TCOR2)
#define TCNT(n) (n == 0 ? TCNT0 : n == 1 ? TCNT1 : TCNT2)
#define TCR(n) (n == 0 ? TCR0 : n == 1 ? TCR1 : TCR2)
#define TUNI(n) \
  (n == 0 ? SH4_INTC_TUNI0 : n == 1 ? SH4_INTC_TUNI1 : SH4_INTC_TUNI2)

void SH4::UpdateTimerStart() {
  for (int i = 0; i < 3; i++) {
    TimerHandle &handle = tmu_timers_[i];

    if (TSTR(i)) {
      // schedule the timer if not already started
      if (handle == INVALID_TIMER) {
        RescheduleTimer(i, TCNT(i), TCR(i));
      }
    } else if (handle != INVALID_TIMER) {
      // disable the timer
      scheduler_->CancelTimer(handle);
      handle = INVALID_TIMER;
    }
  }
}

void SH4::UpdateTimerControl(uint32_t n) {
  if (TSTR(n)) {
    // timer is already scheduled, reschedule it with the current cycle count,
    // but the new TCR value
    RescheduleTimer(n, TimerCount(n), TCR(n));
  }

  // if the timer no longer cares about underflow interrupts, unrequest
  if (!(TCR(n) & 0x20) || !(TCR(n) & 0x100)) {
    UnrequestInterrupt(TUNI(n));
  }
}

void SH4::UpdateTimerCount(uint32_t n) {
  if (TSTR(n)) {
    RescheduleTimer(n, TCNT(n), TCR(n));
  }
}

uint32_t SH4::TimerCount(int n) {
  // TCNT values aren't updated in real time. if a timer is enabled, query the
  // scheduler to figure out how many cycles are remaining for the given timer
  if (!TSTR(n)) {
    return TCNT(n);
  }

  // FIXME should the number of SH4 cycles that've been executed be considered
  // here? this would prevent an entire SH4 slice from just busy waiting on
  // this to change

  TimerHandle &handle = tmu_timers_[n];
  uint32_t tcr = TCR(n);

  int64_t freq = PERIPHERAL_CLOCK_FREQ >> PERIPHERAL_SCALE[tcr & 7];
  std::chrono::nanoseconds remaining = scheduler_->RemainingTime(handle);
  int64_t cycles = NANO_TO_CYCLES(remaining, freq);

  return static_cast<uint32_t>(cycles);
}

void SH4::RescheduleTimer(int n, uint32_t tcnt, uint32_t tcr) {
  TimerHandle &handle = tmu_timers_[n];

  int64_t freq = PERIPHERAL_CLOCK_FREQ >> PERIPHERAL_SCALE[tcr & 7];
  int64_t cycles = static_cast<int64_t>(tcnt);
  std::chrono::nanoseconds remaining = CYCLES_TO_NANO(cycles, freq);

  if (handle) {
    scheduler_->CancelTimer(handle);
    handle = nullptr;
  }

  handle = scheduler_->ScheduleTimer(tmu_delegates_[n], remaining);
}

template <int N>
void SH4::ExpireTimer() {
  uint32_t &tcor = TCOR(N);
  uint32_t &tcnt = TCNT(N);
  uint32_t &tcr = TCR(N);

  // timer expired, set the underflow flag
  tcr |= 0x100;

  // if interrupt generation on underflow is enabled, do so
  if (tcr & 0x20) {
    RequestInterrupt(TUNI(N));
  }

  // reset TCNT with the value from TCOR
  tcnt = tcor;

  // reschedule the timer with the new count
  RescheduleTimer(N, tcnt, tcr);
}

SH4_R32_DELEGATE(PDTRA) {
  // magic values to get past 0x8c00b948 in the boot rom:
  // void _8c00b92c(int arg1) {
  //   sysvars->var1 = reg[PDTRA];
  //   for (i = 0; i < 4; i++) {
  //     sysvars->var2 = reg[PDTRA];
  //     if (arg1 == sysvars->var2 & 0x03) {
  //       return;
  //     }
  //   }
  //   reg[PR] = (uint32_t *)0x8c000000;    /* loop forever */
  // }
  // old_PCTRA = reg[PCTRA];
  // i = old_PCTRA | 0x08;
  // reg[PCTRA] = i;
  // reg[PDTRA] = reg[PDTRA] | 0x03;
  // _8c00b92c(3);
  // reg[PCTRA] = i | 0x03;
  // _8c00b92c(3);
  // reg[PDTRA] = reg[PDTRA] & 0xfffe;
  // _8c00b92c(0);
  // reg[PCTRA] = i;
  // _8c00b92c(3);
  // reg[PCTRA] = i | 0x04;
  // _8c00b92c(3);
  // reg[PDTRA] = reg[PDTRA] & 0xfffd;
  // _8c00b92c(0);
  // reg[PCTRA] = old_PCTRA;
  uint32_t pctra = PCTRA;
  uint32_t pdtra = PDTRA;
  uint32_t v = 0;
  if ((pctra & 0xf) == 0x8 || ((pctra & 0xf) == 0xb && (pdtra & 0xf) != 0x2) ||
      ((pctra & 0xf) == 0xc && (pdtra & 0xf) == 0x2)) {
    v = 3;
  }
  // FIXME cable setting
  // When a VGA cable* is connected
  // 1. The SH4 obtains the cable information from the PIO port.  (PB[9:8] =
  // "00")
  // 2. Set the HOLLY synchronization register for VGA.  (The SYNC output is
  // H-Sync and V-Sync.)
  // 3. When VREG1 = 0 and VREG0 = 0 are written in the AICA register,
  // VIDEO1 = 0 and VIDEO0 = 1 are output.  VIDEO0 is connected to the
  // DVE-DACH pin, and handles switching between RGB and NTSC/PAL.
  //
  // When an RGB(NTSC/PAL) cable* is connected
  // 1. The SH4 obtains the cable information from the PIO port.  (PB[9:8] =
  // "10")
  // 2. Set the HOLLY synchronization register for NTSC/PAL.  (The SYNC
  // output is H-Sync and V-Sync.)
  // 3. When VREG1 = 0 and VREG0 = 0 are written in the AICA register,
  // VIDEO1 = 1 and VIDEO0 = 0 are output.  VIDEO0 is connected to the
  // DVE-DACH pin, and handles switching between RGB and NTSC/PAL.
  //
  // When a stereo A/V cable, an S-jack cable* or an RF converter* is
  // connected
  // 1. The SH4 obtains the cable information from the PIO port.  (PB[9:8] =
  // "11")
  // 2. Set the HOLLY synchronization register for NTSC/PAL.  (The SYNC
  // output is H-Sync and V-Sync.)
  // 3. When VREG1 = 1 and VREG0 = 1 are written in the AICA register,
  // VIDEO1 = 0 and VIDEO0 = 0 are output.  VIDEO0 is connected to the
  // DVE-DACH pin, and handles switching between RGB and NTSC/PAL.
  // v |= 0x3 << 8;
  return v;
}

SH4_W32_DELEGATE(MMUCR) {
  if (!reg.value) {
    return;
  }

  LOG_FATAL("MMU not currently supported");
}

SH4_W32_DELEGATE(CCR) {
  if (CCR.ICI) {
    ResetCache();
  }
}

SH4_W32_DELEGATE(CHCR0) { CheckDMA(0); }

SH4_W32_DELEGATE(CHCR1) { CheckDMA(1); }

SH4_W32_DELEGATE(CHCR2) { CheckDMA(2); }

SH4_W32_DELEGATE(CHCR3) { CheckDMA(3); }

SH4_W32_DELEGATE(DMAOR) {
  CheckDMA(0);
  CheckDMA(1);
  CheckDMA(2);
  CheckDMA(3);
}

SH4_W32_DELEGATE(IPRA) { ReprioritizeInterrupts(); }

SH4_W32_DELEGATE(IPRB) { ReprioritizeInterrupts(); }

SH4_W32_DELEGATE(IPRC) { ReprioritizeInterrupts(); }

SH4_W32_DELEGATE(TSTR) { UpdateTimerStart(); }

SH4_W32_DELEGATE(TCR0) { UpdateTimerControl(0); }

SH4_W32_DELEGATE(TCR1) { UpdateTimerControl(1); }

SH4_W32_DELEGATE(TCR2) { UpdateTimerControl(2); }

SH4_R32_DELEGATE(TCNT0) { return TimerCount(0); }

SH4_W32_DELEGATE(TCNT0) { UpdateTimerCount(0); }

SH4_R32_DELEGATE(TCNT1) { return TimerCount(1); }

SH4_W32_DELEGATE(TCNT1) { UpdateTimerCount(1); }

SH4_R32_DELEGATE(TCNT2) { return TimerCount(2); }

SH4_W32_DELEGATE(TCNT2) { UpdateTimerCount(2); }
