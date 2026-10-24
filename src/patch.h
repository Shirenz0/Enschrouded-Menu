#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace patch {

// First match of a byte pattern with wildcards ("48 8B ?? * 05") in enshrouded.exe's .text, or 0.
// Logs how many matches exist so ambiguous patterns show up in menu.log.
uintptr_t scan(const char* name, const char* pattern);

// Executable memory within +-2 GB of `target` so a 5-byte jmp rel32 can reach it.
uint8_t* allocNear(uintptr_t target, size_t size);

// Writes code bytes while all other threads of the process are suspended and none of them is
// executing inside [addr, addr+n). Returns false if that could not be guaranteed.
bool writeCode(uintptr_t addr, const void* bytes, size_t n);

// Tiny x64 emitter for code caves: raw bytes, rip-relative operands and absolute jumps.
class Emitter {
public:
    explicit Emitter(uint8_t* base) : base_(base) {}

    Emitter& bytes(std::initializer_list<uint8_t> b);
    Emitter& imm32(uint32_t v);
    Emitter& f32(float v);
    // Emits `prefix` followed by a rel32 that addresses `target` relative to the next instruction.
    // `trailing` = immediate bytes that follow the displacement (they count toward "next").
    Emitter& ripRel(std::initializer_list<uint8_t> prefix, const void* target, int trailing = 0);
    // jmp qword ptr [rip+0] ; dq target  (14 bytes, reaches anywhere)
    Emitter& jmpAbs(uintptr_t target);

    // Short conditional/unconditional jump with a forward label: `size_t j = jcc(0x75); ... bind(j);`
    size_t jcc(uint8_t opcode);
    void bind(size_t fixup);

    uint8_t* cursor() const { return base_ + code_.size(); }
    // Copies the emitted bytes into the cave.
    void commit() const;

private:
    uint8_t* base_;
    std::vector<uint8_t> code_;
};

} // namespace patch
