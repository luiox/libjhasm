// assemble/disassemble 往返 smoke：内嵌 JAsm 源（与 printer 输出同版式）覆盖
// 字段/注解/构造器/分支/switch/异常表/invokedynamic/宽常量/new，锁三条契约：
//   1. assemble → analyze 可全量 promote 并产出帧；
//   2. disassemble 文本是规范形式：assemble(T0) 的反汇编 == T0，且字节稳定；
//   3. 真实 classfile 语料（test/smoke/corpus）文本稳定往返。
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jhasm/jhasm.hpp"

#include <libca/str/utf16_string.hpp>
#include <libca/str/utf8_string.hpp>

namespace {

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (condition) {
        printf("[ok]   %s\n", what.c_str());
    }
    else {
        printf("[FAIL] %s\n", what.c_str());
        ++failures;
    }
}

ca::str::Utf16String utf16s(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string narrow(const ca::str::Utf16StringRef value)
{
    return value.to_utf8_string().to_std_string();
}

//版式与 JvmPrinter 输出一致：类级指令在 .class 之前，注解在成员声明之前。
const char* kSource = R"jasm(.sourcefile "Hello.jasm"
.visible-annotation Ldemo/Marker; { value: "hello", count: 7, kinds: { "a", "b" }, color: 'x', flag: true, big: 42L }
.super java/lang/Object
.implements java/lang/Runnable
.class public super demo/Hello {

    .field private static final COUNT I { value: 42 }
    .field private name Ljava/lang/String;

    .method public <init> ()V {
        code: {
            aload 0
            invokespecial java/lang/Object.<init> ()V
            aload 0
            ldc "hi"
            putfield demo/Hello.name Ljava/lang/String;
            return
        }
    }

    .visible-annotation Ldemo/Traced; { }
    .method public run ()V {
        code: {
            getstatic java/lang/System.out Ljava/io/PrintStream;
            ldc "hello world"
            invokevirtual java/io/PrintStream.println (Ljava/lang/String;)V
            return
        }
    }

    .method public static choose (I)I {
        code: {
        L0:
            iload 0
            lookupswitch { 0: L3, 5: L4, default: L5 }
        L3:
            iconst_1
            ireturn
        L4:
            bipush 10
            ireturn
        L5:
            iconst_0
            tableswitch { min: 0, max: 2, cases: { L6, L6, L6 }, default: L6 }
        L6:
            iconst_m1
            ireturn
        }
    }

    .method public static guarded (Ljava/lang/Object;)Ljava/lang/Object; {
        exceptions: { { L0, L1, L2, java/lang/Exception } },
        code: {
        L0:
            aload 0
        L1:
            areturn
        L2:
            pop
            aconst_null
            areturn
        }
    }

    .method public static lambda ()Ljava/lang/Runnable; {
        code: {
            aconst_null
            invokedynamic apply (Ljava/lang/Object;)Ljava/lang/Runnable; { invokestatic, java/lang/invoke/LambdaMetafactory.metafactory, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } { }
            areturn
        }
    }

    .method public static clamp (II)I {
        code: {
            iload 0
            iload 1
            if_icmpgt L0
            iload 0
            ireturn
        L0:
            iload 1
            ireturn
        }
    }

    .method public static constants ()J {
        code: {
            ldc 2147483647
            i2l
            ldc 12345678901234L
            ladd
            ldc 2.718281828459045D
            d2l
            ladd
            lreturn
        }
    }

    .method public static makeObject ()Ljava/lang/Object; {
        code: {
            new java/lang/Object
            dup
            invokespecial java/lang/Object.<init> ()V
            areturn
        }
    }
}
)jasm";

std::vector<ca::u8> slurp(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<ca::u8>((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
}

void checkBytesEqual(const std::vector<ca::u8>& expected, const std::vector<ca::u8>& actual,
                     const std::string& tag)
{
    if (expected == actual) {
        printf("[ok]   %s byte-stable\n", tag.c_str());
        return;
    }
    ++failures;
    printf("[FAIL] %s bytes differ (expected %zu, got %zu)\n", tag.c_str(), expected.size(),
           actual.size());
    for (ca::usize i = 0; i < expected.size() && i < actual.size(); ++i) {
        if (expected[i] != actual[i]) {
            printf("       first diff at offset %zu: %02x vs %02x\n", i, expected[i], actual[i]);
            return;
        }
    }
}

///@brief disassemble → assemble → disassemble：断言文本稳定，返回再装配 bytes。
bool textStable(const std::vector<ca::u8>& bytes, std::vector<ca::u8>& rebuilt,
                const std::string& tag)
{
    auto text0 = jhasm::disassemble(bytes);
    if (!text0.has_value()) {
        printf("[FAIL] %s disassemble\n", tag.c_str());
        for (const auto& error : text0.errors())
            printf("       %s\n", narrow(error.message()).c_str());
        return false;
    }
    const std::string t0 = narrow(text0.get()->ref());
    auto reassembled = jhasm::assemble(utf16s(tag), text0.get()->ref());
    if (!reassembled.has_value()) {
        printf("[FAIL] %s reassemble:\n", tag.c_str());
        for (const auto& error : reassembled.errors())
            printf("       %s\n", narrow(error.message()).c_str());
        printf("---- text ----\n%s\n", t0.c_str());
        return false;
    }
    rebuilt = reassembled.get()->bytes;
    auto text1 = jhasm::disassemble(rebuilt);
    if (!text1.has_value()) {
        printf("[FAIL] %s second disassemble\n", tag.c_str());
        for (const auto& error : text1.errors())
            printf("       %s\n", narrow(error.message()).c_str());
        return false;
    }
    const bool stable = text1.get()->ref() == text0.get()->ref();
    if (!stable) {
        printf("[FAIL] %s text not stable\n---- first ----\n%s\n---- second ----\n%s\n",
               tag.c_str(), t0.c_str(), narrow(text1.get()->ref()).c_str());
    }
    return stable;
}


//JVM 加载门载体（#3 形态去特征）：try 范围只覆盖不可抛指令（iload/ifeq/
//goto），handler 段首为 nop；handler 本体只有异常边进入。装配-反汇编往返
//不验 JVM 语义，SMT 缺 handler 入口帧类缺陷只有真实类加载能拦。
const char* kGateSource = R"jasm(.sourcefile "Gate.jasm"
.super java/lang/Object
.class public super Gate {

    .method public static probe (I)V {
        exceptions: { { L0, L1, L3, java/lang/Exception } },
        code: {
        L0:
            iload 0
            ifeq L2
        L1:
            goto L2
        L3:
            nop
            pop
            return
        L2:
            return
        }
    }

    .method public static main ([Ljava/lang/String;)V {
        code: {
            iconst_0
            invokestatic Gate.probe (I)V
            return
        }
    }
}
)jasm";

}   //namespace

//孤立代理语料：一个常量携带孤立低代理（U+DC31 的 CESU-8 三字节形），
//另一个携带合法代理对（U+1F037）——锁「孤立落 \uXXXX、成对保持原样」
//的双形态契约。
const char* kLoneSource = R"jasm(.super java/lang/Object
.class public super Lone {

    .method public static shout ()V {
        code: {
            ldc "\uDC31"
            pop
            ldc "\uD83B\uDC37"
            pop
            return
        }
    }
}
)jasm";

int main(int argc, char** argv)
{
    //1. assemble
    auto source = utf16s(kSource);
    auto assembled = jhasm::assemble(utf16s("Hello.jasm"), source.ref());
    if (!assembled.has_value()) {
        printf("[FAIL] assemble:\n");
        for (const auto& error : assembled.errors())
            printf("       %s\n", narrow(error.message()).c_str());
        return 1;
    }
    const std::vector<ca::u8>& bytes = assembled.get()->bytes;
    check(!bytes.empty(), "assemble produced bytes");

    //2. analyze：全部方法可 promote 并产出帧
    auto analyzed = jhasm::analyze(bytes);
    if (analyzed.has_value()) {
        check(analyzed.get()->methods.size() == 8, "analyze covers eight methods");
        ca::usize frames = 0;
        for (const auto& method : analyzed.get()->methods)
            for (const auto& frame : method.frames)
                if (frame.has_value()) ++frames;
        check(frames > 0, "analyze produced frames");
        for (const auto& error : analyzed.errors())
            printf("       analyze error: %s\n", narrow(error.message()).c_str());
    }
    else {
        check(false, "analyze");
        for (const auto& error : analyzed.errors())
            printf("       %s\n", narrow(error.message()).c_str());
    }

    //3. 文本规范形式 + 字节稳定
    std::vector<ca::u8> rebuilt;
    if (textStable(bytes, rebuilt, "inline")) {
        checkBytesEqual(bytes, rebuilt, "inline");
    }

    //3b. 孤立代理（lone surrogate）：CP Utf8 携带 CESU-8 代理三字节形时，
    //    Utf16String 承载不了孤立单元的标准 UTF-8 重编码（to_utf8_string
    //    显式拒绝）——文本面落 \uXXXX 转义（合法代理对保持增补平面字符
    //    呈现），装配侧 unescape + modifiedUtf8 逐位还原，往返字节稳定。
    {
        auto lone = jhasm::assemble(utf16s("Lone.jasm"), utf16s(kLoneSource).ref());
        if (!lone.has_value()) {
            check(false, "lone surrogate assemble");
            for (const auto& error : lone.errors())
                printf("       %s\n", narrow(error.message()).c_str());
        }
        else {
            const std::vector<ca::u8>& loneBytes = lone.get()->bytes;
            bool stored = false;
            for (ca::usize i = 0; i + 2 < loneBytes.size(); ++i) {
                if (loneBytes[i] == 0xED && loneBytes[i + 1] == 0xB0 &&
                    loneBytes[i + 2] == 0xB1) {
                    stored = true;
                    break;
                }
            }
            check(stored, "lone surrogate stored as CESU-8 ED B0 B1");
            std::vector<ca::u8> loneRebuilt;
            if (textStable(loneBytes, loneRebuilt, "lone")) {
                checkBytesEqual(loneBytes, loneRebuilt, "lone");
            }
        }
    }

    //4. 真实 classfile 语料：文本稳定往返；javac 常量池编排与本项目写入器
    //   不同序，逐字节对齐原文件不可达，改验自幂等——重建 bytes 再过一轮
    //   必须逐字节一致。
    const std::string corpus = argc > 1 ? argv[1] : "test/smoke/corpus/test_class.class";
    auto classBytes = slurp(corpus);
    if (!classBytes.empty()) {
        std::vector<ca::u8> corpusRebuilt;
        if (textStable(classBytes, corpusRebuilt, "corpus")) {
            std::vector<ca::u8> idempotent;
            if (textStable(corpusRebuilt, idempotent, "corpus2"))
                checkBytesEqual(corpusRebuilt, idempotent, "corpus");
        }
    }
    else {
        printf("[skip] corpus %s missing\n", corpus.c_str());
    }

    //5. JVM 加载门：装配产物经 java -Xverify:all 真实加载验收。java 缺席
    //   时跳过（JDK 缺失环境不阻塞其余 smoke 契约）。
    {
        auto gate = jhasm::assemble(utf16s("Gate.jasm"), utf16s(kGateSource).ref());
        if (!gate.has_value()) {
            check(false, "gate assemble");
            for (const auto& error : gate.errors())
                printf("       %s\n", narrow(error.message()).c_str());
        }
        else {
            const int javaProbe =
#ifdef _WIN32
                std::system("java -version >nul 2>&1");
#else
                std::system("java -version >/dev/null 2>&1");
#endif
            if (javaProbe != 0) {
                printf("[skip] java not on PATH; JVM load gate skipped\n");
            }
            else {
                namespace fs = std::filesystem;
                const fs::path dir = fs::temp_directory_path() / "jhasm_gate_smoke";
                std::error_code ec;
                fs::remove_all(dir, ec);
                fs::create_directories(dir);
                {
                    std::ofstream out(dir / "Gate.class", std::ios::binary);
                    out.write(reinterpret_cast<const char*>(gate.get()->bytes.data()),
                              static_cast<std::streamsize>(gate.get()->bytes.size()));
                }
                const std::string cmd =
                    "java -Xverify:all -cp " + dir.string() + " Gate";
                const int rc = std::system(cmd.c_str());
                check(rc == 0, "jvm load gate (java -Xverify:all)");
                fs::remove_all(dir, ec);
            }
        }
    }

    printf(failures == 0 ? "SMOKE PASS\n" : "SMOKE FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
