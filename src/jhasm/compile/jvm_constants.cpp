#include "jvm_constants.hpp"

#include <cstring>
#include <limits>
#include <vector>

namespace jhasm::compile {

namespace {

IR::MethodHandleSpec makeHandle(ca::u32 kind, const std::string& path,
                                const std::string& descriptor, SpecArena& arena)
{
    const auto separator = path.rfind('.');
    if (separator == std::string::npos) fail("Handle path has no member name");
    IR::MethodHandleSpec spec;
    spec.referenceKind = kind;
    const auto stored  = arena.put(path);
    spec.owner         = stored.substr(0, separator);
    spec.name          = stored.substr(separator + 1);
    spec.desc          = arena.put(descriptor);
    if (kind <= H_PUTSTATIC) {
        if (jbc::type::parseFieldType(spec.desc).is_err()) fail("Invalid field descriptor");
    }
    else if (jbc::type::parseMethodDescriptor(spec.desc).is_err()) {
        fail("Invalid method descriptor");
    }
    return spec;
}

IR::MethodHandleSpec shortcutHandle(ca::str::Utf16StringRef name, SpecArena& arena)
{
    if (equals(name, "LambdaMetafactory.metafactory")) {
        return makeHandle(H_INVOKESTATIC,
                          "java/lang/invoke/LambdaMetafactory.metafactory",
                          "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/"
                          "invoke/MethodType;"
                          "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/"
                          "invoke/MethodType;)"
                          "Ljava/lang/invoke/CallSite;",
                          arena);
    }
    fail("Unknown method handle shortcut");
}

ConstSpec numberConstant(const ast::Number& number)
{
    ConstSpec result;
    if (number.isFloatingPoint()) {
        if (number.isWide()) {
            result.kind  = ConstSpec::Kind::Double;
            result.dbits = doubleWords(floatingValue(number)).first;
        }
        else {
            result.kind  = ConstSpec::Kind::Float;
            result.fbits = floatBits(static_cast<ca::f32>(floatingValue(number)));
        }
    }
    else if (number.isWide()) {
        result.kind = ConstSpec::Kind::Long;
        result.num  = integerValue(number);
    }
    else {
        result.kind = ConstSpec::Kind::Int;
        result.num  = integerValue(number);
    }
    return result;
}

}   //namespace

IR::MethodHandleSpec methodHandle(const ast::ElementPtr& value, SpecArena& arena)
{
    if (!value) fail("Missing method handle");
    if (value->getType() == ast::ElementKind::IDENTIFIER)
        return shortcutHandle(value->content(), arena);
    if (value->getType() != ast::ElementKind::ARRAY) fail("Invalid method handle");
    const auto& parts = std::static_pointer_cast<ast::Array>(value)->values();
    if (parts.size() != 3) fail("Invalid method handle arity");
    ca::u32     kind     = 0;
    const auto  kindName = parts[0]->content();
    if (equals(kindName, "getfield"))
        kind = H_GETFIELD;
    else if (equals(kindName, "getstatic"))
        kind = H_GETSTATIC;
    else if (equals(kindName, "putfield"))
        kind = H_PUTFIELD;
    else if (equals(kindName, "putstatic"))
        kind = H_PUTSTATIC;
    else if (equals(kindName, "invokevirtual"))
        kind = H_INVOKEVIRTUAL;
    else if (equals(kindName, "invokestatic"))
        kind = H_INVOKESTATIC;
    else if (equals(kindName, "invokespecial"))
        kind = H_INVOKESPECIAL;
    else if (equals(kindName, "newinvokespecial"))
        kind = H_NEWINVOKESPECIAL;
    else if (equals(kindName, "invokeinterface"))
        kind = H_INVOKEINTERFACE;
    else
        fail("Unknown method handle kind");
    return makeHandle(kind, narrow(*parts[1]), narrow(*parts[2]), arena);
}

ConstSpec constantValue(const ast::ElementPtr& value, SpecArena& arena)
{
    if (!value) fail("Missing constant");
    switch (value->getType()) {
    case ast::ElementKind::NUMBER: return numberConstant(*std::static_pointer_cast<ast::Number>(value));
    case ast::ElementKind::STRING:
    {
        ConstSpec result;
        result.kind = ConstSpec::Kind::String;
        result.str  = arena.put(narrow(*value));
        return result;
    }
    case ast::ElementKind::CHARACTER:
    {
        ConstSpec result;
        result.kind = ConstSpec::Kind::Int;
        result.num  = value->content().empty() ? 0 : value->content().char_at(0).value;
        return result;
    }
    case ast::ElementKind::BOOL:
    {
        ConstSpec result;
        result.kind = ConstSpec::Kind::Int;
        result.num  = std::static_pointer_cast<ast::Bool>(value)->boolValue() ? 1 : 0;
        return result;
    }
    case ast::ElementKind::IDENTIFIER:
    {
        auto text = value->content();
        if (equals(text, "true") || equals(text, "false")) {
            ConstSpec result;
            result.kind = ConstSpec::Kind::Int;
            result.num  = equals(text, "true") ? 1 : 0;
            return result;
        }
        if (equals(text, "NaN") || equals(text, "NaND")) {
            ConstSpec result;
            result.kind  = ConstSpec::Kind::Double;
            result.dbits = doubleWords(std::numeric_limits<ca::f64>::quiet_NaN()).first;
            return result;
        }
        if (equals(text, "NaNF")) {
            ConstSpec result;
            result.kind  = ConstSpec::Kind::Float;
            result.fbits = floatBits(std::numeric_limits<ca::f32>::quiet_NaN());
            return result;
        }
        if (equals(text, "Infinity") || equals(text, "+Infinity") || equals(text, "InfinityD") ||
            equals(text, "+InfinityD")) {
            ConstSpec result;
            result.kind  = ConstSpec::Kind::Double;
            result.dbits = doubleWords(std::numeric_limits<ca::f64>::infinity()).first;
            return result;
        }
        if (equals(text, "-Infinity") || equals(text, "-InfinityD")) {
            ConstSpec result;
            result.kind  = ConstSpec::Kind::Double;
            result.dbits = doubleWords(-std::numeric_limits<ca::f64>::infinity()).first;
            return result;
        }
        if (equals(text, "InfinityF") || equals(text, "+InfinityF")) {
            ConstSpec result;
            result.kind  = ConstSpec::Kind::Float;
            result.fbits = floatBits(std::numeric_limits<ca::f32>::infinity());
            return result;
        }
        if (equals(text, "-InfinityF")) {
            ConstSpec result;
            result.kind  = ConstSpec::Kind::Float;
            result.fbits = floatBits(-std::numeric_limits<ca::f32>::infinity());
            return result;
        }
        const auto descriptor = narrow(*value);
        if (!descriptor.empty() && descriptor.front() == '(') {
            //MethodType 常量（indy BSM 参数族）：方法描述符整串落
            //CONSTANT_MethodType（parseFieldType 不收 '(' 形，分道解析）。
            if (jbc::type::parseMethodDescriptor(descriptor).is_err())
                fail("Invalid method descriptor");
            ConstSpec result;
            result.kind = ConstSpec::Kind::MethodType;
            result.str  = arena.put(descriptor);
            return result;
        }
        if (!descriptor.empty() &&
            (descriptor.front() == 'L' || descriptor.front() == '[')) {
            if (jbc::type::parseFieldType(descriptor).is_err()) fail("Invalid type descriptor");
            ConstSpec result;
            result.kind = ConstSpec::Kind::Class;
            result.str  = arena.put(descriptor.front() == '['
                                        ? descriptor
                                        : descriptor.substr(1, descriptor.size() - 2));
            return result;
        }
        {
            ConstSpec result;
            result.kind   = ConstSpec::Kind::MethodHandle;
            result.handle = shortcutHandle(text, arena);
            return result;
        }
    }
    case ast::ElementKind::ARRAY:
    {
        const auto& parts = std::static_pointer_cast<ast::Array>(value)->values();
        if (parts.size() == 3) {
            ConstSpec result;
            result.kind   = ConstSpec::Kind::MethodHandle;
            result.handle = methodHandle(value, arena);
            return result;
        }
        if (parts.size() != 4) fail("Invalid dynamic constant");
        const auto descriptor = narrow(*parts[1]);
        const auto parsed     = jbc::type::parseFieldType(descriptor);
        if (parsed.is_err()) fail("Invalid dynamic constant descriptor");
        ConstSpec result;
        result.kind          = ConstSpec::Kind::Dynamic;
        result.dynamicName   = arena.put(narrow(*parts[0]));
        result.dynamicDesc   = arena.put(descriptor);
        result.dynamicHandle = methodHandle(parts[2], arena);
        if (parts[3]->getType() == ast::ElementKind::ARRAY) {
            for (const auto& item : std::static_pointer_cast<ast::Array>(parts[3])->values())
                result.dynamicArgs.push_back(constantValue(item, arena));
        }
        return result;
    }
    default: fail("Unsupported constant kind");
    }
}

ca::u32 internConstant(IR::ClassSession& session, const ConstSpec& value)
{
    switch (value.kind) {
    case ConstSpec::Kind::Int: return session.internInteger(static_cast<ca::i32>(value.num));
    case ConstSpec::Kind::Long: return session.internLong(value.num);
    case ConstSpec::Kind::Float: return session.internFloatBits(value.fbits);
    case ConstSpec::Kind::Double: return session.internDoubleBits(value.dbits);
    case ConstSpec::Kind::String: return session.internString(value.str);
    case ConstSpec::Kind::Class: return session.internClass(value.str);
    case ConstSpec::Kind::MethodType: return session.internMethodType(value.str);
    case ConstSpec::Kind::MethodHandle:
        return require(session.internMethodHandle(value.handle), "internMethodHandle");
    case ConstSpec::Kind::Dynamic:
    {
        std::vector<IR::BsmArg> args;
        args.reserve(value.dynamicArgs.size());
        for (const auto& item : value.dynamicArgs) args.push_back(bsmArgOf(session, item));
        const auto bsm = require(
            session.appendBootstrapMethod(value.dynamicHandle, args), "appendBootstrapMethod");
        return require(session.internDynamic(bsm, value.dynamicName, value.dynamicDesc),
                       "internDynamic");
    }
    }
    fail("Unreachable constant kind");
}

IR::Insn constantLoad(IR::ClassSession& session, const ConstSpec& value)
{
    IR::Insn in;
    switch (value.kind) {
    case ConstSpec::Kind::Int:
        in.op = C::Op::LDC;
        in.cp = session.internInteger(static_cast<ca::i32>(value.num));
        break;
    case ConstSpec::Kind::Long:
    case ConstSpec::Kind::Double:
        in.op = C::Op::LDC2_W;
        in.cp = value.kind == ConstSpec::Kind::Long ? session.internLong(value.num)
                                                    : session.internDoubleBits(value.dbits);
        break;
    case ConstSpec::Kind::Float:
        in.op = C::Op::LDC;
        in.cp = session.internFloatBits(value.fbits);
        break;
    default:
        in.op = C::Op::LDC;
        in.cp = internConstant(session, value);
        break;
    }
    if (in.op == C::Op::LDC && in.cp > 0xFF) in.op = C::Op::LDC_W;
    return in;
}

IR::BsmArg bsmArgOf(IR::ClassSession& session, const ConstSpec& value)
{
    IR::BsmArg result;
    switch (value.kind) {
    case ConstSpec::Kind::Int:
    case ConstSpec::Kind::Long:
        result.kind = value.kind == ConstSpec::Kind::Long ? IR::BsmArg::Kind::Long
                                                          : IR::BsmArg::Kind::Int;
        result.num  = value.num;
        return result;
    case ConstSpec::Kind::Float:
        result.kind  = IR::BsmArg::Kind::Float;
        result.fbits = value.fbits;
        return result;
    case ConstSpec::Kind::Double:
        result.kind  = IR::BsmArg::Kind::Double;
        result.dbits = value.dbits;
        return result;
    case ConstSpec::Kind::String:
        result.kind = IR::BsmArg::Kind::String;
        result.str  = value.str;
        return result;
    case ConstSpec::Kind::Class:
        result.kind = IR::BsmArg::Kind::Class;
        result.str  = value.str;
        return result;
    case ConstSpec::Kind::MethodType:
        result.kind = IR::BsmArg::Kind::MethodType;
        result.str  = value.str;
        return result;
    case ConstSpec::Kind::MethodHandle:
        result.kind   = IR::BsmArg::Kind::MethodHandle;
        result.handle = value.handle;
        return result;
    case ConstSpec::Kind::Dynamic:
    {
        std::vector<IR::BsmArg> nested;
        nested.reserve(value.dynamicArgs.size());
        for (const auto& item : value.dynamicArgs) nested.push_back(bsmArgOf(session, item));
        const auto bsm = require(
            session.appendBootstrapMethod(value.dynamicHandle, nested), "appendBootstrapMethod");
        result.kind = IR::BsmArg::Kind::Dynamic;
        result.bsm  = bsm;
        result.name = value.dynamicName;
        result.desc = value.dynamicDesc;
        return result;
    }
    }
    fail("Unreachable constant kind");
}

}   //namespace jhasm::compile
