#include "jvm_compiler.hpp"

#include "code_compiler.hpp"
#include "jvm_annotations.hpp"
#include "jvm_compile_detail.hpp"
#include "jvm_constants.hpp"

#include <jbc/jbc.hpp>

#include <libca/str/utf8_string.hpp>

#include <memory>
#include <string>
#include <vector>

namespace jhasm::compile {

namespace {

namespace C  = jbc::core;
namespace IR = jbc::ir;

std::vector<ca::u8> compileClass(const ast::Class& clazz, const JvmCompilerOptions& options)
{
    const std::string thisName = internalNameOf(*clazz.name());
    const std::string superName =
        clazz.superName() ? internalNameOf(*clazz.superName()) : std::string{};
    std::vector<std::string> interfaceNames;
    for (const auto& value : clazz.interfaces())
        interfaceNames.push_back(internalNameOf(*value));

    IR::NewClassSeed seed;
    seed.version = (static_cast<ca::u32>(options.majorVersion) << 16) | options.minorVersion;
    seed.access  = access(clazz.modifiers());
    seed.thisName = thisName;
    seed.superName = superName;
    seed.interfaces = std::vector<std::string_view>{interfaceNames.begin(), interfaceNames.end()};
    auto session = require(IR::makeSession(seed), "makeSession");

    //类级属性（挂载顺序与旧栈打印面的出现顺序对齐，方便文本对照）。
    if (auto signature = signatureAttr(*session, clazz.attributes()))
        require(session->attachClassAttr("Signature", std::move(signature)), "attachClassAttr");
    if (clazz.sourceFile()) {
        auto source   = std::make_unique<C::SourceFileAttr>();
        source->sourceFileCp = session->internUtf8(narrow(*clazz.sourceFile()));
        require(session->attachClassAttr("SourceFile", std::move(source)), "attachClassAttr");
    }
    if (clazz.outerClass()) {
        auto enclosing = std::make_unique<C::EnclosingMethodAttr>();
        enclosing->classCp = session->internClass(internalNameOf(*clazz.outerClass()));
        if (clazz.outerMethod()) {
            enclosing->methodNatCp = session->backing().cp.addNameAndType(
                session->internUtf8(narrow(*clazz.outerMethod()->name())),
                session->internUtf8(narrow(*clazz.outerMethod()->descriptor())));
        }
        require(session->attachClassAttr("EnclosingMethod", std::move(enclosing)),
                "attachClassAttr");
    }
    if (clazz.nestHost()) {
        auto nest      = std::make_unique<C::NestHostAttr>();
        nest->hostClassCp = session->internClass(internalNameOf(*clazz.nestHost()));
        require(session->attachClassAttr("NestHost", std::move(nest)), "attachClassAttr");
    }
    if (!clazz.nestMembers().empty()) {
        auto nest = std::make_unique<C::NestMembersAttr>();
        for (const auto& value : clazz.nestMembers())
            nest->memberCps.push_back(session->internClass(internalNameOf(*value)));
        require(session->attachClassAttr("NestMembers", std::move(nest)), "attachClassAttr");
    }
    if (!clazz.permittedSubclasses().empty()) {
        auto permitted = std::make_unique<C::PermittedSubclassesAttr>();
        for (const auto& value : clazz.permittedSubclasses())
            permitted->permittedCps.push_back(session->internClass(internalNameOf(*value)));
        require(session->attachClassAttr("PermittedSubclasses", std::move(permitted)),
                "attachClassAttr");
    }
    if (!clazz.innerClasses().empty()) {
        auto inner = std::make_unique<C::InnerClassesAttr>();
        for (const auto& value : clazz.innerClasses()) {
            C::InnerClassesAttr::Row row;
            row.innerClassInfoCp = session->internClass(internalNameOf(*value->innerClass()));
            row.outerClassInfoCp =
                value->outerClass() ? session->internClass(internalNameOf(*value->outerClass())) : 0;
            row.innerNameCp = value->name() ? session->internUtf8(narrow(*value->name())) : 0;
            row.accessFlags = static_cast<ca::u16>(access(value->modifiers()));
            inner->rows.push_back(row);
        }
        require(session->attachClassAttr("InnerClasses", std::move(inner)), "attachClassAttr");
    }
    if (!clazz.recordComponents().empty()) {
        auto record = std::make_unique<C::RecordAttr>();
        for (const auto& value : clazz.recordComponents()) {
            C::RecordAttr::Component component;
            component.nameCp        = session->internUtf8(narrow(*value->name()));
            component.descriptorCp  = session->internUtf8(narrow(*value->descriptor()));
            mountComponentAttrs(*session, component.attrs, value->attributes());
            record->components.push_back(std::move(component));
        }
        require(session->attachClassAttr("Record", std::move(record)), "attachClassAttr");
    }
    installAnnotations(
        *session, clazz.attributes(),
        [s = session.get()](bool visible, const std::vector<IR::SemAnnotation>& annos) {
            return s->attachClassAnnotations(visible, annos);
        },
        [s = session.get()](std::string_view name, std::unique_ptr<C::AttrBody> body) {
            return s->attachClassAttr(name, std::move(body));
        });

    for (const auto& item : clazz.contents()) {
        if (item->getType() == ast::ElementKind::FIELD) {
            const auto field = std::static_pointer_cast<ast::Field>(item);
            //Seed 名字/描述符是 string_view：先落命名串再挂，视图必须活过 addField。
            const std::string fieldName = narrow(*field->name());
            const std::string fieldDesc = narrow(*field->descriptor());
            IR::FieldSeed fieldSeed;
            fieldSeed.name   = fieldName;
            fieldSeed.desc   = fieldDesc;
            fieldSeed.access = access(field->modifiers());
            if (jbc::type::parseFieldType(fieldSeed.desc).is_err())
                fail("Invalid field descriptor");
            const auto id = require(session->addField(fieldSeed), "addField");
            if (auto signature = signatureAttr(*session, field->attributes()))
                require(session->attachAttr(id, "Signature", std::move(signature)),
                        "attachAttr");
            if (field->fieldValue()) {
                SpecArena arena;
                const auto constant = constantValue(field->fieldValue(), arena);
                IR::ConstValueSpec value;
                switch (constant.kind) {
                case ConstSpec::Kind::Int:
                    value.kind = IR::ConstValueSpec::Kind::Int;
                    value.num  = constant.num;
                    break;
                case ConstSpec::Kind::Long:
                    value.kind = IR::ConstValueSpec::Kind::Long;
                    value.num  = constant.num;
                    break;
                case ConstSpec::Kind::Float:
                    value.kind  = IR::ConstValueSpec::Kind::Float;
                    value.fbits = constant.fbits;
                    break;
                case ConstSpec::Kind::Double:
                    value.kind  = IR::ConstValueSpec::Kind::Double;
                    value.dbits = constant.dbits;
                    break;
                case ConstSpec::Kind::String:
                    value.kind = IR::ConstValueSpec::Kind::String;
                    value.str  = constant.str;
                    break;
                default: fail("Unsupported field constant kind");
                }
                require(session->attachConstantValue(id, value), "attachConstantValue");
            }
            installAnnotations(
                *session, field->attributes(),
                [s = session.get(), id](bool visible, const std::vector<IR::SemAnnotation>& annos) {
                    return s->attachAnnotations(id, visible, annos);
                },
                [s = session.get(), id](std::string_view name, std::unique_ptr<C::AttrBody> body) {
                    return s->attachAttr(id, name, std::move(body));
                });
        }
        else if (item->getType() == ast::ElementKind::METHOD) {
            const auto method = std::static_pointer_cast<ast::Method>(item);
            std::string name = narrow(*method->name());
            std::string descriptor = narrow(*method->descriptor());
            if (jbc::type::parseMethodDescriptor(descriptor).is_err())
                fail("Invalid method descriptor");
            const bool isStatic = (access(method->modifiers()) & ACC_STATIC) != 0;
            const bool withCode = method->code() != nullptr;

            IR::MethodSeed methodSeed;
            methodSeed.name   = name;
            methodSeed.desc   = descriptor;
            methodSeed.access = access(method->modifiers());
            methodSeed.withCode = withCode;
            methodSeed.maxLocals = static_cast<ca::u16>(
                paramSlotTotal(descriptor, isStatic));
            const auto id = require(session->addMethod(methodSeed), "addMethod");

            if (auto signature = signatureAttr(*session, method->attributes()))
                require(session->attachAttr(id, "Signature", std::move(signature)), "attachAttr");
            installAnnotations(
                *session, method->attributes(),
                [s = session.get(), id](bool visible, const std::vector<IR::SemAnnotation>& annos) {
                    return s->attachAnnotations(id, visible, annos);
                },
                [s = session.get(), id](std::string_view name, std::unique_ptr<C::AttrBody> body) {
                    return s->attachAttr(id, name, std::move(body));
                });
            if (!method->parameters().empty()) {
                std::vector<IR::ParamNameRow> rows;
                rows.reserve(method->parameters().size());
                std::vector<std::string> parameterNames;
                parameterNames.reserve(method->parameters().size());
                for (const auto& parameter : method->parameters()) {
                    //ParamNameRow.name 是 string_view：先落驻留串再挂行。
                    parameterNames.push_back(narrow(*parameter));
                    rows.push_back(IR::ParamNameRow{parameterNames.back(), 0});
                }
                require(session->attachMethodParameters(id, rows), "attachMethodParameters");
            }
            if (method->defaultValue()) {
                SpecArena arena;
                require(session->attachAnnotationDefault(id, semElement(method->defaultValue(),
                                                                        arena)),
                        "attachAnnotationDefault");
            }

            if (withCode) {
                auto promoted = IR::promoteMethod(*session, id);
                if (promoted.is_err()) fail("Method body promote failed");
                IR::CodeUnitRef unit = std::move(promoted).unwrap();
                CodeCompiler compiler(*session, unit, *method, descriptor);
                compiler.compile();
                finishBody(*session, id, *unit, compiler.lineRows(), options, name, descriptor,
                           isStatic);
            }
        }
    }

    auto bytes = require(C::writeClass(session->backing()), "writeClass");
    return bytes;
}
}   //namespace

error::Result<JvmCompileResult> JvmCompiler::compile(
    const std::vector<ast::ElementPtr>& input) const
{
    const ast::Class* clazz = nullptr;
    for (const auto& item : input) {
        if (item && item->getType() == ast::ElementKind::CLASS) {
            if (clazz) {
                auto message =
                    ca::str::Utf16String::value_of("Expected exactly one class declaration");
                return error::Result<JvmCompileResult>::err({error::Error(message.ref())});
            }
            clazz = static_cast<const ast::Class*>(item.get());
        }
    }
    if (!clazz) {
        auto message = ca::str::Utf16String::value_of("Expected a class declaration");
        return error::Result<JvmCompileResult>::err({error::Error(message.ref())});
    }
    return compile(*clazz);
}

error::Result<JvmCompileResult> JvmCompiler::compile(const ast::Class& clazz) const
{
    try {
        auto bytes = compileClass(clazz, options_);
        return error::Result<JvmCompileResult>::ok({std::move(bytes)});
    }
    catch (const std::exception& exception) {
        auto                      message  = util::utf16(exception.what());
        const util::Location*     location = clazz.location();
        std::vector<error::Error> errors;
        if (location)
            errors.emplace_back(message.ref(), *location);
        else
            errors.emplace_back(message.ref());
        return error::Result<JvmCompileResult>::err(std::move(errors));
    }
}

}   //namespace jhasm::compile
