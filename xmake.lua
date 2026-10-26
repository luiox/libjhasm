-- libjhasm：JVM 汇编文本（JAsm）解析/分析/编译/打印库（独立构建根）。
--
-- 依赖：
--   - libjbc（项目内联 git 直连包，xmake/packages/libjbc.lua）：字节码
--     core/ir/analysis/type 面——解析、从零构建、帧分析与保真写回的唯一后端。
--     类型不出现在公开头（实现层私有依赖），下游无需传递引入。
--   - libca（luiox-repo git 直连包）：字符串/基础面（public 传递）。
--
-- 旧栈（libjblw + libjbytecode）已整体退役：编译/分析/打印三面全部改走
-- libjbc（见 git 历史与 jhasm.hpp 注释）。
--
-- 仓库注册：本文件不声明 add_repositories——包沙箱会继承宿主的仓库注册，
-- 在工程内重复注册同名仓库会导致嵌套包构建时 remote 路径错乱。独立开发/CI
-- 先执行：
--   xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git

-- libca 版本/模块双单源：与内联 libjbc 包定义的 add_deps 共用同一字面量
-- （定义文件在常量之后 include）。模块集 = libjbc 工程自取集（core,str,
-- crypto,zip），保证同一消费图内 libca 只有一个实例。
LUIOX_LIBCA_VERSION = "0.0.16"
LUIOX_LIBCA_MODULES = "core,str,crypto,zip"

includes("xmake/packages/libjbc.lua")

set_languages("cxx17")
add_rules("mode.debug", "mode.release")

add_requires("libjbc 0.4.17")
add_requires("libca " .. LUIOX_LIBCA_VERSION, {configs = {modules = LUIOX_LIBCA_MODULES}})

option("with_tests")
    set_default(false)
    set_showmenu(true)
    set_description("Enable unit tests (pulls gtest)")
option_end()

option("with_smoke")
    set_default(false)
    set_showmenu(true)
    set_description("Enable assemble/disassemble round-trip smoke test (standalone, no gtest)")
option_end()

target("libjhasm")
    set_kind("static")
    -- libjbc 不以 public 挂载：公开头零 jbc 类型，实现层私有。
    add_packages("libjbc")
    add_packages("libca", {public = true})
    add_includedirs("src", {public = true})
    add_files("src/jhasm/**.cpp")
    add_headerfiles("src/(jhasm/**.hpp)")
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

if has_config("with_smoke") then
    target("libjhasm_smoke")
        set_kind("binary")
        add_deps("libjhasm")
        add_files("test/smoke/roundtrip_smoke.cpp")
        if is_plat("windows") then
            add_cxflags("/utf-8", {tools = "cl"})
        end
end
