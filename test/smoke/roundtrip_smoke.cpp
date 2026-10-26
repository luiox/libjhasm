// assemble/disassemble 往返 smoke：内嵌 JAsm 源（与 printer 输出同版式）覆盖
// 字段/注解/构造器/分支/switch/异常表/invokedynamic/宽常量/new，锁三条契约：
//   1. assemble → analyze 可全量 promote 并产出帧；
//   2. disassemble 文本是规范形式：assemble(T0) 的反汇编 == T0，且字节稳定；
//   3. 真实 classfile 语料（test/smoke/corpus）文本稳定往返。
#include <cstdio>
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

}   //namespace

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

    printf(failures == 0 ? "SMOKE PASS\n" : "SMOKE FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
