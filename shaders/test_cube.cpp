// Synthetic ALU instructions only: no game data in these regression tests.
#include "XenosRecomp/shader_recompiler.h"
#include <fstream>
#include <stdexcept>

static void EmptyTableTranslation() {
    std::vector<uint8_t> data(96 + 4096);
    const auto put = [&](size_t at, uint32_t value) {
        for (size_t i=0;i<4;++i) data[at+i]=uint8_t(value>>(24-i*8));
    };
    put(0,0x102a1101); put(4,72); put(8,24); put(24,36);
    put(36,0); put(40,24); put(72,0x1001); put(76,0x2000);
    ShaderRecompiler compiler;
    compiler.recompile(data.data(), "");
    if (compiler.out.find("void main(") == std::string::npos)
        throw std::runtime_error("minimal shader with no CTAB did not translate");
    if (compiler.out.find("iVertexId : SV_VertexID")==std::string::npos ||
        compiler.out.find("float(iVertexId & 0xFFFFFFu)")==std::string::npos)
        throw std::runtime_error("vertex entry r0 does not contain guest vertex index");
}

static void VertexInputsTranslation() {
    std::vector<uint8_t> data(128 + 4096);
    const auto put = [&](size_t at, uint32_t value) {
        for (size_t i=0;i<4;++i) data[at+i]=uint8_t(value>>(24-i*8));
    };
    put(0,0x102a1101); put(4,104); put(8,24); put(24,36);
    put(36,0); put(40,24); put(64,4);
    // Two fetch addresses share POSITION0; NORMAL1 still needs a location.
    put(72,0); put(76,1); put(80,2 | (3u<<12) | (1u<<16));
    put(84,3 | (5u<<12)); put(104,0x1001); put(108,0x2000);
    ShaderRecompiler compiler;
    compiler.recompile(data.data(), "");
    const auto& out=compiler.out;
    const std::string position="in float4 iPosition0 : POSITION0,";
    auto first=out.find(position);
    if (first==std::string::npos || out.find(position, first+1)!=std::string::npos)
        throw std::runtime_error("repeated vertex fetch emits duplicate parameter");
    if (out.find("[[vk::location(16)]] in float4 iNormal1 : NORMAL1,")==std::string::npos)
        throw std::runtime_error("NORMAL1 has no explicit location");
    if (compiler.vertexElements.size()!=4)
        throw std::runtime_error("deduplication lost a fetch address");
}

static void ParallelAluTranslation() {
    AluInstruction instruction{};
    instruction.vectorOpcode=AluVectorOpcode::Add;
    instruction.vectorWriteMask=15;
    instruction.vectorDest=0;
    instruction.scalarOpcode=AluScalarOpcode::Adds;
    instruction.scalarWriteMask=1;
    instruction.src1Select=instruction.src2Select=instruction.src3Select=1;
    instruction.src3Register=0;
    ShaderRecompiler compiler;
    compiler.recompile(instruction);
    const auto& out=compiler.out;
    if (out.find("float4 aluVectorSource = r0;")==std::string::npos ||
        out.find("aluVectorSource.")==std::string::npos)
        throw std::runtime_error("scalar lane reads vector destination after overwrite");
}

static std::string Translate(AluInstruction instruction, bool declared = false) {
    ShaderRecompiler compiler;
    const char name[] = "Directions";
    ConstantInfo constant{};
    if (declared) {
        constant.registerIndex.value = byteSwap(uint16_t(248));
        constant.registerCount.value = byteSwap(uint16_t(8));
        compiler.constantTableData = reinterpret_cast<const uint8_t*>(name);
        compiler.float4Constants.emplace(250, &constant);
    }
    compiler.recompile(instruction);
    return compiler.out;
}

static void Expect(const std::string& output, const char* expected) {
    if (output != expected)
        throw std::runtime_error("unexpected CUBE translation: " + output);
}

int main(int argc, char** argv) try {
    if (argc != 2) throw std::runtime_error("usage: test_cube <new HLSL output>");
    EmptyTableTranslation();
    VertexInputsTranslation();
    ParallelAluTranslation();
    AluInstruction instruction{};
    instruction.vectorOpcode = AluVectorOpcode::Cube;
    instruction.scalarOpcode = AluScalarOpcode::RetainPrev;
    instruction.vectorWriteMask = 15;
    instruction.src1Register = 5;
    instruction.src1Select = 1;
    instruction.src1Swizzle = 0xAA;  // relative swizzle encoding of ZWXY
    const auto ordinary = Translate(instruction);
    Expect(ordinary, "r0.xyzw = cube((r5.zwxy).zwxy, cubeMapData);\n");
    instruction.src1Register = 0x85;  // abs(r5), not register 133
    instruction.src1Negate = 1;
    const auto signed_temp = Translate(instruction);
    Expect(signed_temp, "r0.xyzw = cube((-abs(r5.zwxy)).zwxy, cubeMapData);\n");
    instruction.src1Select = 0;
    instruction.src1Register = 250;
    instruction.absConstants = 1;
    const auto literal = Translate(instruction);
    Expect(literal, "r0.xyzw = cube((-abs(c250.zwxy)).zwxy, cubeMapData);\n");
    instruction.const0Relative = 1;
    instruction.constAddressRegisterRelative = 1;
    const auto relative = Translate(instruction, true);
    Expect(relative, "r0.xyzw = cube((-abs(Directions(2 + a0).zwxy)).zwxy, cubeMapData);\n");
    instruction.constAddressRegisterRelative = 0;
    Expect(Translate(instruction, true),
        "r0.xyzw = cube((-abs(Directions(2 + aL).zwxy)).zwxy, cubeMapData);\n");
    // Unsupported relative constants must fail, rather than silently read c250.
    bool rejected = false;
    try { (void)Translate(instruction); } catch (const std::runtime_error&) { rejected = true; }
    if (!rejected) throw std::runtime_error("undeclared relative constant was accepted");
    instruction.const0Relative = 0;
    instruction.src1Negate = 0;
    instruction.absConstants = 0;
    instruction.vectorWriteMask = 9;
    const auto masked = Translate(instruction);
    Expect(masked, "r0.xw = cube((c250.zwxy).zwxy, cubeMapData).xw;\n");
    instruction.src1Swizzle = 0;
    const auto swizzled = Translate(instruction);
    Expect(swizzled, "r0.xw = cube((c250.xyzw).zwxy, cubeMapData).xw;\n");
    std::ofstream file(argv[1]);
    file << R"(struct CubeMapData { uint index; };
float4 cube(float4 value, inout CubeMapData data) { ++data.index; return value; }
#define Directions(INDEX) directions[INDEX]
float4 main() : SV_Target0 {
  float4 r0 = 0, r5 = float4(-1, 2, -3, 4), c250 = r5;
  float4 directions[8];
  [unroll] for (int i = 0; i < 8; ++i) directions[i] = r5 + i;
  int a0 = 1, aL = 2;
  CubeMapData cubeMapData = (CubeMapData)0;
)" << ordinary << signed_temp << literal << relative << masked << swizzled
         << "return r0;\n}\n";
    if (!file.flush()) throw std::runtime_error("could not write regression HLSL");
    std::printf("8 CUBE operand cases passed\n");
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
