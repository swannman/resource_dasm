#include <inttypes.h>
#include <string.h>

#include <filesystem>
#include <map>
#include <memory>
#include <phosg/Arguments.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Random.hh>
#include <phosg/Strings.hh>
#include <phosg/Tools.hh>
#include <random>
#include <stdexcept>
#include <vector>

#include "Emulators/M68KEmulator.hh"
#include "Emulators/PPC32Emulator.hh"
#include "Emulators/SH4Emulator.hh"
#include "Emulators/X86Emulator.hh"

bool run_68k_emulator_test(uint32_t start_opcode = 0, uint32_t end_opcode = 0x10000) {
  std::mt19937 gen(0x11213380);

  std::string mem_data;
  mem_data.reserve(0x20060);
  while (mem_data.size() < 0x20060) {
    uint32_t v = gen();
    mem_data.push_back(v >> 24);
    mem_data.push_back(v >> 16);
    mem_data.push_back(v >> 8);
    mem_data.push_back(v);
  }
  phosg::StringReader mem_data_r(mem_data);

  // Set up base memory
  uint32_t mem_base = 0x11213380;
  uint32_t code_base = 0xA0000000;
  auto mem = std::make_shared<ResourceDASM::MemoryContext>();
  mem->allocate_at(mem_base, mem_data.size());
  mem->memcpy(mem_base, mem_data.data(), mem_data.size());
  mem->allocate_at(code_base, 0x20);

  ResourceDASM::M68KEmulator::Regs base_regs;
  for (size_t z = 0; z < 8; z++) {
    base_regs.a[z] = mem_base + 0x10000 + (z * 4);
    base_regs.d[z].u = mem_base + 0x10040 + (z * 4);
    base_regs.pc = code_base;
    base_regs.sr.u = 0;
  }

  struct OpcodeWord {
    uint16_t v = 0;
    bool low_byte_random = false;
    bool high_byte_random = false;
  };
  std::vector<OpcodeWord> opcode{OpcodeWord{.v = static_cast<uint16_t>(start_opcode)}};

  ResourceDASM::M68KEmulator emu(mem);
  ssize_t random_values_remaining = -1;
  while (!opcode.empty()) {
    std::string data_str;
    for (size_t z = 0; z < opcode.size(); z++) {
      uint16_t v = opcode[z].v;
      mem->write_u16b(code_base + z * 2, v);
      data_str += std::format("{:04X}", v);
    }
    if (random_values_remaining >= 0) {
      data_str += std::format(" (+{})", random_values_remaining);
    }
    mem->memset(code_base + (opcode.size() * 2), 0, 0x20 - (opcode.size() * 2));

    auto disassembly = ResourceDASM::M68KEmulator::disassemble_one_structured(
        mem->at<void>(code_base, opcode.size() * 2), opcode.size() * 2);
    if (disassembly.segments.size() != 1) {
      throw std::logic_error("disassembly did not produce exactly one segment");
    }
    const auto& seg = disassembly.segments[0];
    if (seg.disassembly.contains(".incomplete")) {
      phosg::fwritex(stdout, phosg::format_color_escape(phosg::TerminalFormat::FG_YELLOW, phosg::TerminalFormat::BOLD, phosg::TerminalFormat::END));
      phosg::fwrite_fmt(stdout, "{}  {} ...\n", data_str, seg.disassembly);
      phosg::fwritex(stdout, phosg::format_color_escape(phosg::TerminalFormat::NORMAL, phosg::TerminalFormat::END));
      opcode.emplace_back(OpcodeWord{.v = 0x0000});
      continue;
    } else if (seg.size < opcode.size() * 2) {
      opcode.resize(seg.size / 2);
    }

    if ((random_values_remaining == -1) && !seg.imm_offsets.empty()) {
      random_values_remaining = 8;
      for (const auto& [offset, size] : seg.imm_offsets) {
        for (size_t z = 0; z < size; z++) {
          auto& b = opcode.at((offset + z) / 2);
          if ((offset + z) & 1) {
            b.low_byte_random = true;
          } else {
            b.high_byte_random = true;
          }
        }
      }
    }

    if (!seg.is_valid) {
      phosg::fwritex(stdout, phosg::format_color_escape(phosg::TerminalFormat::FG_YELLOW, phosg::TerminalFormat::BOLD, phosg::TerminalFormat::END));
      phosg::fwrite_fmt(stdout, "{}  {} ...\n", data_str, seg.disassembly);
      phosg::fwritex(stdout, phosg::format_color_escape(phosg::TerminalFormat::NORMAL, phosg::TerminalFormat::END));

    } else {
      // Run the opcode with various CCR values
      for (uint8_t ccr_set = 0; ccr_set < 2; ccr_set++) {
        auto& emu_regs = emu.registers();
        base_regs.sr.u = ccr_set ? 0x1F : 0x00;
        emu_regs = base_regs;

        phosg::fwritex(stdout, phosg::format_color_escape(phosg::TerminalFormat::FG_WHITE, phosg::TerminalFormat::BOLD, phosg::TerminalFormat::END));
        phosg::fwrite_fmt(stdout, "{} (/{:02X})  {} =>", data_str, base_regs.sr.u, seg.disassembly);

        try {
          emu.execute_one();
        } catch (const std::runtime_error& e) {
          phosg::fwritex(stdout, phosg::format_color_escape(phosg::TerminalFormat::FG_YELLOW, phosg::TerminalFormat::BOLD, phosg::TerminalFormat::END));
          phosg::fwrite_fmt(stdout, " ERR:[{}]", e.what());
        } catch (const std::out_of_range& e) {
          phosg::fwritex(stdout, phosg::format_color_escape(phosg::TerminalFormat::FG_YELLOW, phosg::TerminalFormat::BOLD, phosg::TerminalFormat::END));
          phosg::fwrite_fmt(stdout, " ERR:[{}]", e.what());
        }

        phosg::fwritex(stdout, phosg::format_color_escape(phosg::TerminalFormat::FG_WHITE, phosg::TerminalFormat::BOLD, phosg::TerminalFormat::END));
        for (size_t z = 0; z < 8; z++) {
          if (base_regs.a[z] != emu_regs.a[z]) {
            phosg::fwrite_fmt(stdout, " A{}:{:08X}->{:08X}", z, base_regs.a[z], emu_regs.a[z]);
          }
        }
        for (size_t z = 0; z < 8; z++) {
          if (base_regs.d[z].u != emu_regs.d[z].u) {
            phosg::fwrite_fmt(stdout, " D{}:{:08X}->{:08X}", z, base_regs.d[z].u, emu_regs.d[z].u);
          }
        }
        if (base_regs.sr.u != emu_regs.sr.u) {
          phosg::fwrite_fmt(stdout, " SR:{:04X}->{:04X}", base_regs.sr.u, emu_regs.sr.u);
        }
        for (size_t z = 0; z < mem_data.size(); z += 4) {
          uint32_t base_val = mem_data_r.pget_u32b(z);
          uint32_t mem_val = mem->read_u32b(mem_base + z);
          if (base_val != mem_val) {
            phosg::fwrite_fmt(stdout, " {:08X}:{:08X}->{:08X}", mem_base + z, base_val, mem_val);
            mem->write_u32b(mem_base + z, base_val);
          }
        }
        phosg::fwritex(stdout, phosg::format_color_escape(phosg::TerminalFormat::NORMAL, phosg::TerminalFormat::END));
        fputc('\n', stdout);
      }
    }

    // Advance to the next opcode
    if (random_values_remaining >= 0) {
      bool last_value = ((random_values_remaining--) == 0);
      for (auto& b : opcode) {
        if (b.low_byte_random && b.high_byte_random) {
          b.v = last_value ? 0xFFFF : gen();
        } else if (b.low_byte_random) {
          b.v = (b.v & 0xFF00) | (last_value ? 0xFF : (gen() & 0xFF));
        } else if (b.high_byte_random) {
          b.v = (b.v & 0x00FF) | (last_value ? 0xFF00 : (gen() & 0xFF00));
        }
        if (last_value) {
          b.high_byte_random = false;
          b.low_byte_random = false;
        }
      }
    }

    if (random_values_remaining < 0) {
      while (opcode.back().v == 0xFFFF) {
        opcode.pop_back();
      }
      if (!opcode.empty()) {
        opcode.back().v++;
      }
      if ((opcode.size() == 1) && (opcode[0].v >= end_opcode)) {
        break;
      }
    }
  }

  return true;
}

// Checks that M68KEmulator's cached block execution is indistinguishable from the classic loop: same registers,
// memory and cycle count, the same view from an interrupt callback, self-modifying code observed immediately, and
// the debug hook called exactly where the block contract says it is.
bool run_68k_block_execution_test() {
  static constexpr uint32_t CODE = 0x00100000;
  static constexpr uint32_t DATA = 0x00800000;
  static const std::vector<uint16_t> program{
      /* +00 */ 0x203C, 0x0000, 0x03E8, // move.l  d0, 1000
      /* +06 */ 0x7200, //                 moveq   d1, 0
      /* +08 */ 0x41F9, 0x0080, 0x0000, // lea     a0, [DATA]
      /* +0E */ 0xD280, //                 add.l   d1, d0             (loop head)
      /* +10 */ 0x2081, //                 move.l  [a0], d1
      /* +12 */ 0x6100, 0x0010, //         bsr.w   +24
      /* +16 */ 0x5380, //                 subq.l  d0, 1
      /* +18 */ 0x66F4, //                 bne.s   +0E
      /* +1A */ 0xA9F4, //                 syscall (the handler ends the run)
      /* +1C */ 0x4E71, 0x4E71, 0x4E71, 0x4E71,
      /* +24 */ 0x5281, //                 addq.l  d1, 1              (patched to addq.l d1, 2 when d0 == 500)
      /* +26 */ 0x0C80, 0x0000, 0x01F4, // cmpi.l  d0, 500
      /* +2C */ 0x6608, //                 bne.s   +36
      /* +2E */ 0x33FC, 0x5481, 0x0010, 0x0024, // move.w [CODE + 24], 0x5481
      /* +36 */ 0x4E75, //                 rts
  };

  struct Result {
    ResourceDASM::M68KEmulator::Regs regs;
    uint64_t cycles = 0;
    uint32_t stored = 0;
    uint64_t hook_calls = 0;
    std::map<uint32_t, uint64_t> hook_calls_at;
    uint32_t interrupt_pc = 0;
    uint32_t interrupt_d1 = 0;
  };
  enum class Mode {
    CLASSIC,
    BLOCKS,
    BLOCKS_WITH_BOUNDARY,
    BLOCKS_HOOK_EVERY_INSTRUCTION,
  };

  auto run = [&](Mode mode) -> Result {
    auto mem = std::make_shared<ResourceDASM::MemoryContext>();
    mem->allocate_at(CODE, 0x1000);
    mem->allocate_at(DATA, 0x1000);
    for (size_t z = 0; z < program.size(); z++) {
      mem->write_u16b(CODE + z * 2, program[z]);
    }

    Result ret;
    ResourceDASM::M68KEmulator emu(mem);
    auto& regs = emu.registers();
    regs.pc = CODE;
    // The stack shares the code's page on purpose: every bsr then bumps that page's write generation, so every cached
    // instruction is revalidated against its bytes on every pass — the case real 68K programs put the cache in.
    regs.a[7] = CODE + 0x0800;
    emu.set_syscall_handler([](ResourceDASM::M68KEmulator&, uint16_t) {
      throw ResourceDASM::M68KEmulator::terminate_emulation();
    });
    emu.set_debug_hook([&](ResourceDASM::M68KEmulator& e) {
      ret.hook_calls++;
      ret.hook_calls_at[e.registers().pc]++;
    });
    auto im = std::make_shared<ResourceDASM::InterruptManager>();
    im->add(5000, [&]() -> bool {
      ret.interrupt_pc = emu.registers().pc;
      ret.interrupt_d1 = emu.registers().d[1].u;
      return true;
    });
    emu.set_interrupt_manager(im);

    if (mode != Mode::CLASSIC) {
      emu.set_block_execution(true);
    }
    if (mode == Mode::BLOCKS_WITH_BOUNDARY) {
      emu.add_block_boundary_pc(CODE + 0x26);
    }
    if (mode == Mode::BLOCKS_HOOK_EVERY_INSTRUCTION) {
      emu.set_hook_every_instruction(true);
    }
    emu.execute();

    ret.regs = emu.registers();
    ret.cycles = emu.cycles();
    ret.stored = mem->read_u32b(DATA);
    return ret;
  };

  bool ok = true;
  auto expect_eq = [&](const char* mode, const char* what, uint64_t actual, uint64_t expected) {
    if (actual != expected) {
      phosg::fwrite_fmt(stderr, "block execution test FAILED: [{}] {} is {} (0x{:X}); expected {} (0x{:X})\n",
          mode, what, actual, actual, expected, expected);
      ok = false;
    }
  };

  auto classic = run(Mode::CLASSIC);
  // 501 passes add 1 in the subroutine before the patch lands (d0 = 1000..500) and 499 passes add 2 after it
  expect_eq("classic", "d1", classic.regs.d[1].u, 500500 + 501 + 2 * 499);
  expect_eq("classic", "cycles", classic.cycles, 3 + 9 * 1000 + 1);
  expect_eq("classic", "hook calls", classic.hook_calls, classic.cycles + 1); // +1: the terminating syscall

  auto check_same_as_classic = [&](const char* mode, const Result& r) {
    for (size_t z = 0; z < 8; z++) {
      expect_eq(mode, "a data register", r.regs.d[z].u, classic.regs.d[z].u);
      expect_eq(mode, "an address register", r.regs.a[z], classic.regs.a[z]);
    }
    expect_eq(mode, "pc", r.regs.pc, classic.regs.pc);
    expect_eq(mode, "sr", r.regs.sr.u, classic.regs.sr.u);
    expect_eq(mode, "cycles", r.cycles, classic.cycles);
    expect_eq(mode, "stored value", r.stored, classic.stored);
    expect_eq(mode, "pc seen by the interrupt callback", r.interrupt_pc, classic.interrupt_pc);
    expect_eq(mode, "d1 seen by the interrupt callback", r.interrupt_d1, classic.interrupt_d1);
  };
  auto calls_at = [](const Result& r, uint32_t offset) -> uint64_t {
    auto it = r.hook_calls_at.find(CODE + offset);
    return (it == r.hook_calls_at.end()) ? 0 : it->second;
  };

  auto blocks = run(Mode::BLOCKS);
  check_same_as_classic("blocks", blocks);
  // Block entries: the first instruction, the bsr target, the rts landing, both outcomes of each bne
  expect_eq("blocks", "hook calls", blocks.hook_calls, 1 + 1000 + 1000 + 999 + 1 + 999 + 1);
  expect_eq("blocks", "hook calls at the loop head", calls_at(blocks, 0x0E), 999);
  expect_eq("blocks", "hook calls at the subroutine entry", calls_at(blocks, 0x24), 1000);
  expect_eq("blocks", "hook calls at the return landing", calls_at(blocks, 0x16), 1000);
  expect_eq("blocks", "hook calls mid-block", calls_at(blocks, 0x26), 0);

  auto boundary = run(Mode::BLOCKS_WITH_BOUNDARY);
  check_same_as_classic("blocks+boundary", boundary);
  expect_eq("blocks+boundary", "hook calls", boundary.hook_calls, blocks.hook_calls + 1000);
  expect_eq("blocks+boundary", "hook calls at the boundary", calls_at(boundary, 0x26), 1000);

  auto every = run(Mode::BLOCKS_HOOK_EVERY_INSTRUCTION);
  check_same_as_classic("blocks+hook-every-instruction", every);
  expect_eq("blocks+hook-every-instruction", "hook calls", every.hook_calls, classic.hook_calls);
  if (every.hook_calls_at != classic.hook_calls_at) {
    phosg::fwrite_fmt(stderr,
        "block execution test FAILED: [blocks+hook-every-instruction] per-pc hook calls differ\n");
    ok = false;
  }

  if (ok) {
    phosg::fwrite_fmt(stderr, "block execution test passed\n");
  }
  return ok;
}

int main(int argc, char** argv) {
  phosg::Arguments args(argv + 1, argc - 1);

  if (args.get<bool>("test-expression")) {
    auto expr = ResourceDASM::Expression::Node::parse(args.get<std::string>(0));
    phosg::fwrite_fmt(stderr, "Expression: {}\n", expr->str());
    phosg::fwrite_fmt(stderr, "Result: {} ({})\n", expr->evaluate().str(), expr->evaluate().str(true));
    return 0;

  } else if (args.get<bool>("test-assemble-ppc32")) {
    return !ResourceDASM::PPC32Emulator::test_assembler(
        args.get<size_t>("threads", 0),
        args.get<uint32_t>("start-opcode", 0, phosg::Arguments::IntFormat::HEX),
        args.get<bool>("stop-on-failure"),
        args.get<bool>("verbose"));

  } else if (args.get<bool>("test-assemble-sh4")) {
    return !ResourceDASM::SH4Emulator::test_assembler(args.get<bool>("stop-on-failure"), args.get<bool>("verbose"));

  } else if (args.get<bool>("test-assemble-x86")) {
    return !ResourceDASM::X86Emulator::test_assembler(
        phosg::parse_data_string(args.get<std::string>("start-opcode", false)),
        args.get<bool>("stop-on-failure"),
        args.get<bool>("verbose"));

  } else if (args.get<bool>("test-block-execution-68k")) {
    return !run_68k_block_execution_test();

  } else if (args.get<bool>("test-exec-68k")) {
    return !run_68k_emulator_test(
        args.get<uint32_t>("start-opcode", 0, phosg::Arguments::IntFormat::HEX),
        args.get<uint32_t>("end-opcode", 0x10000, phosg::Arguments::IntFormat::HEX));
  }

  return 0;
}
