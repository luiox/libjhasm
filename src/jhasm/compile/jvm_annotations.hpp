#pragma once

// jasm 注解面 → libjbc 合成规格（编译面内部头）：简单注解（SemAnnotation，
// 走会话 attach 族）、类型注解（手写 typed 体，直挂 AttrSlot）、Signature 与
// Record 组件嵌套属性表。类/字段/方法三处挂载共用 installAnnotations 形状。

#include "jvm_compile_detail.hpp"

#include <memory>

namespace jhasm::compile {

///@brief AST 节点 → 注解元素值规格（注解默认值 attach 用）。
IR::SemElemValue semElement(const ast::ElementPtr& value, SpecArena& arena);

///@brief AST 注解 → 语义注解规格（借用串落 SpecArena，活到 attach 返回）。
IR::SemAnnotation semAnnotation(const ast::Annotation& value, SpecArena& arena);

template <typename AttachSimple>
void attachSimpleAnnotations(IR::ClassSession& session, bool visible,
                             const std::vector<ast::AnnotationPtr>& values,
                             AttachSimple&& attach)
{
    if (values.empty()) return;
    //arena 活到 attach 返回：SemAnnotation 的借用串都在这里。
    SpecArena             arena;
    std::vector<IR::SemAnnotation> annos;
    annos.reserve(values.size());
    for (const auto& value : values) annos.push_back(semAnnotation(*value, arena));
    require(attach(visible, annos), "attachAnnotations");
}

template <typename AttachType>
void attachTypeAnnotations(IR::ClassSession& session, bool visible,
                           const std::vector<ast::AnnotationPtr>& values, AttachType&& attach)
{
    if (values.empty()) return;
    require(attach(visible ? "RuntimeVisibleTypeAnnotations" : "RuntimeInvisibleTypeAnnotations",
                   typeAnnotationsAttr(session, values)),
            "attachTypeAnnotations");
}

///@brief 简单注解 + 类型注解的成套挂载（类/字段/方法共用形状）。
template <typename AttachSimple, typename AttachType>
void installAnnotations(IR::ClassSession& session, const ast::Attributes& attributes,
                        AttachSimple&& attachSimple, AttachType&& attachType)
{
    attachSimpleAnnotations(session, true, attributes.visibleAnnotations, attachSimple);
    attachSimpleAnnotations(session, false, attributes.invisibleAnnotations, attachSimple);
    attachTypeAnnotations(session, true, attributes.visibleTypeAnnotations, attachType);
    attachTypeAnnotations(session, false, attributes.invisibleTypeAnnotations, attachType);
}

///@brief Signature 属性体（无 .signature 声明时返回空）。
std::unique_ptr<C::SignatureAttr> signatureAttr(IR::ClassSession& session,
                                                const ast::Attributes& attributes);

///@brief Record 组件嵌套属性表直挂（挂载点在 Record 属性体内，不经会话 attach 面）。
void mountComponentAttrs(IR::ClassSession& session, std::vector<C::AttrSlot>& table,
                         const ast::Attributes& attributes);

///@brief 类型注解 AST 列表 → typed TypeAnnotations 属性体（手写体，直挂面）。
std::unique_ptr<C::TypeAnnotationsAttr> typeAnnotationsAttr(
    IR::ClassSession& session, const std::vector<ast::AnnotationPtr>& values);

}   //namespace jhasm::compile
