#include "jvm_annotations.hpp"

#include "jhasm/util/type_annotation_codec.hpp"

#include <memory>
#include <vector>

namespace jhasm::compile {

namespace {

IR::SemElemValue numberElement(const ast::Number& number)
{
    IR::SemElemValue result;
    if (number.isFloatingPoint()) {
        if (number.isWide()) {
            result.form  = IR::SemElemValue::Form::Double;
            result.dbits = doubleWords(floatingValue(number)).first;
        }
        else {
            result.form  = IR::SemElemValue::Form::Float;
            result.fbits = floatBits(static_cast<ca::f32>(floatingValue(number)));
        }
    }
    else if (number.isWide()) {
        result.form = IR::SemElemValue::Form::Long;
        result.num  = integerValue(number);
    }
    else {
        result.form = IR::SemElemValue::Form::Int;
        result.num  = integerValue(number);
    }
    return result;
}

C::AnnotationsAttr::ElemValue rawElement(IR::ClassSession& session, const ast::ElementPtr& value);

C::AnnotationsAttr::Annotation rawAnnotation(IR::ClassSession& session,
                                             const ast::Annotation& value)
{
    C::AnnotationsAttr::Annotation result;
    result.typeCp = session.internUtf8(descriptorOf(*value.classType()));
    for (const auto& entry : value.values())
        result.values.emplace_back(session.internUtf8(narrow(*entry.first)),
                                   rawElement(session, entry.second));
    return result;
}

C::AnnotationsAttr::ElemValue rawElement(IR::ClassSession& session, const ast::ElementPtr& value)
{
    if (!value) fail("Missing annotation element");
    C::AnnotationsAttr::ElemValue result;
    switch (value->getType()) {
    case ast::ElementKind::BOOL:
        result.tag = 'Z';
        result.cp  = session.internInteger(
            std::static_pointer_cast<ast::Bool>(value)->boolValue() ? 1 : 0);
        return result;
    case ast::ElementKind::CHARACTER:
        result.tag = 'C';
        result.cp  = session.internInteger(
            value->content().empty() ? 0 : value->content().char_at(0).value);
        return result;
    case ast::ElementKind::STRING:
        result.tag = 's';
        result.cp  = session.internString(narrow(*value));
        return result;
    case ast::ElementKind::NUMBER:
    {
        const auto number = std::static_pointer_cast<ast::Number>(value);
        if (number->isFloatingPoint()) {
            if (number->isWide()) {
                result.tag  = 'D';
                result.cp   = session.internDoubleBits(doubleWords(floatingValue(*number)).first);
            }
            else {
                result.tag = 'F';
                result.cp  = session.internFloatBits(
                    floatBits(static_cast<ca::f32>(floatingValue(*number))));
            }
        }
        else if (number->isWide()) {
            result.tag = 'J';
            result.cp  = session.internLong(integerValue(*number));
        }
        else {
            result.tag = 'I';
            result.cp  = session.internInteger(static_cast<ca::i32>(integerValue(*number)));
        }
        return result;
    }
    case ast::ElementKind::IDENTIFIER:
        if (equals(value->content(), "true") || equals(value->content(), "false")) {
            result.tag = 'Z';
            result.cp  = session.internInteger(equals(value->content(), "true") ? 1 : 0);
            return result;
        }
        result.tag = 'c';
        result.cp  = session.internUtf8(
            descriptorOf(*std::static_pointer_cast<ast::Identifier>(value)));
        return result;
    case ast::ElementKind::ENUM_VALUE:
    {
        const auto item   = std::static_pointer_cast<ast::EnumValue>(value);
        result.tag        = 'e';
        result.typeNameCp = session.internUtf8(descriptorOf(*item->enumType()));
        result.constNameCp = session.internUtf8(narrow(*item->enumValue()));
        return result;
    }
    case ast::ElementKind::ANNOTATION:
        result.tag    = '@';
        result.nested = std::make_unique<C::AnnotationsAttr::Annotation>(
            rawAnnotation(session, *std::static_pointer_cast<ast::Annotation>(value)));
        return result;
    case ast::ElementKind::ARRAY:
        result.tag = '[';
        for (const auto& item : std::static_pointer_cast<ast::Array>(value)->values())
            result.items.push_back(rawElement(session, item));
        return result;
    default: fail("Unsupported annotation element");
    }
}

C::TypeAnnotationsAttr::Annotation typeAnnotation(IR::ClassSession& session,
                                                  const ast::Annotation& value)
{
    using TA = C::TypeAnnotationsAttr;
    C::TypeAnnotationsAttr::Annotation result;
    ca::u8 targetType = 0;
    ca::u32 first = 0, second = 0;
    util::unpackTypeRef(value.typeRef() ? static_cast<ca::i32>(integerValue(*value.typeRef())) : 0,
                        targetType, first, second);
    result.targetType = targetType;
    switch (targetType) {
    case TA::kClassTypeParameter:
    case TA::kMethodTypeParameter: result.typeParameterIndex = static_cast<ca::u8>(first); break;
    case TA::kSupertype: result.supertypeIndex = first; break;
    case TA::kClassTypeParameterBound:
    case TA::kMethodTypeParameterBound:
        result.typeParameterIndex = static_cast<ca::u8>(first);
        result.boundIndex         = static_cast<ca::u8>(second);
        break;
    case TA::kField:
    case TA::kMethodReturn:
    case TA::kMethodReceiver: break;
    case TA::kFormalsParameter: result.formalParameterIndex = static_cast<ca::u8>(first); break;
    case TA::kThrows: result.throwsIndex = first; break;
    default: fail("Unsupported type annotation target");
    }
    if (value.typePath()) result.typePath = util::parseTypePath(narrow(*value.typePath()));
    result.typeIndexCp = session.internUtf8(descriptorOf(*value.classType()));
    for (const auto& entry : value.values())
        result.values.emplace_back(session.internUtf8(narrow(*entry.first)),
                                   rawElement(session, entry.second));
    return result;
}

}   //namespace

IR::SemElemValue semElement(const ast::ElementPtr& value, SpecArena& arena)
{
    if (!value) fail("Missing annotation element");
    IR::SemElemValue result;
    switch (value->getType()) {
    case ast::ElementKind::BOOL:
        result.form = IR::SemElemValue::Form::Bool;
        result.num  = std::static_pointer_cast<ast::Bool>(value)->boolValue() ? 1 : 0;
        return result;
    case ast::ElementKind::CHARACTER:
        result.form = IR::SemElemValue::Form::Char;
        result.num  = value->content().empty() ? 0 : value->content().char_at(0).value;
        return result;
    case ast::ElementKind::STRING:
        result.form = IR::SemElemValue::Form::String;
        result.str  = arena.put(narrow(*value));
        return result;
    case ast::ElementKind::NUMBER: return numberElement(*std::static_pointer_cast<ast::Number>(value));
    case ast::ElementKind::IDENTIFIER:
        if (equals(value->content(), "true")) {
            result.form = IR::SemElemValue::Form::Bool;
            result.num  = 1;
            return result;
        }
        if (equals(value->content(), "false")) {
            result.form = IR::SemElemValue::Form::Bool;
            result.num  = 0;
            return result;
        }
        result.form = IR::SemElemValue::Form::Class;
        result.str  = arena.put(descriptorOf(*std::static_pointer_cast<ast::Identifier>(value)));
        return result;
    case ast::ElementKind::ENUM_VALUE:
    {
        const auto item = std::static_pointer_cast<ast::EnumValue>(value);
        result.form     = IR::SemElemValue::Form::Enum;
        result.enumType = arena.put(descriptorOf(*item->enumType()));
        result.str      = arena.put(narrow(*item->enumValue()));
        return result;
    }
    case ast::ElementKind::ANNOTATION:
        result.form   = IR::SemElemValue::Form::Nested;
        result.nested = std::make_shared<IR::SemAnnotation>(
            semAnnotation(*std::static_pointer_cast<ast::Annotation>(value), arena));
        return result;
    case ast::ElementKind::ARRAY:
        for (const auto& item : std::static_pointer_cast<ast::Array>(value)->values())
            result.array.push_back(semElement(item, arena));
        result.form = IR::SemElemValue::Form::Array;
        return result;
    default: fail("Unsupported annotation element");
    }
}

IR::SemAnnotation semAnnotation(const ast::Annotation& value, SpecArena& arena)
{
    IR::SemAnnotation result;
    result.typeDesc = arena.put(descriptorOf(*value.classType()));
    for (const auto& entry : value.values())
        result.values.emplace_back(arena.put(narrow(*entry.first)),
                                   semElement(entry.second, arena));
    return result;
}

std::unique_ptr<C::TypeAnnotationsAttr> typeAnnotationsAttr(
    IR::ClassSession& session, const std::vector<ast::AnnotationPtr>& values)
{
    auto body  = std::make_unique<C::TypeAnnotationsAttr>();
    body->annotations.reserve(values.size());
    for (const auto& value : values) body->annotations.push_back(typeAnnotation(session, *value));
    return body;
}

std::unique_ptr<C::SignatureAttr> signatureAttr(IR::ClassSession& session,
                                                const ast::Attributes& attributes)
{
    if (!attributes.signature) return nullptr;
    auto body        = std::make_unique<C::SignatureAttr>();
    body->signatureCp = session.internUtf8(narrow(*attributes.signature));
    return body;
}

void mountComponentAttrs(IR::ClassSession& session, std::vector<C::AttrSlot>& table,
                         const ast::Attributes& attributes)
{
    if (auto signature = signatureAttr(session, attributes))
        table.push_back(C::AttrSlot::makeNew(session.internUtf8("Signature"), std::move(signature)));
    if (!attributes.visibleAnnotations.empty()) {
        auto body = std::make_unique<C::AnnotationsAttr>();
        for (const auto& value : attributes.visibleAnnotations)
            body->annos.push_back(rawAnnotation(session, *value));
        table.push_back(
            C::AttrSlot::makeNew(session.internUtf8("RuntimeVisibleAnnotations"), std::move(body)));
    }
    if (!attributes.invisibleAnnotations.empty()) {
        auto body = std::make_unique<C::AnnotationsAttr>();
        for (const auto& value : attributes.invisibleAnnotations)
            body->annos.push_back(rawAnnotation(session, *value));
        table.push_back(
            C::AttrSlot::makeNew(session.internUtf8("RuntimeInvisibleAnnotations"), std::move(body)));
    }
    if (!attributes.visibleTypeAnnotations.empty())
        table.push_back(C::AttrSlot::makeNew(
            session.internUtf8("RuntimeVisibleTypeAnnotations"),
            typeAnnotationsAttr(session, attributes.visibleTypeAnnotations)));
    if (!attributes.invisibleTypeAnnotations.empty())
        table.push_back(C::AttrSlot::makeNew(
            session.internUtf8("RuntimeInvisibleTypeAnnotations"),
            typeAnnotationsAttr(session, attributes.invisibleTypeAnnotations)));
}

}   //namespace jhasm::compile
