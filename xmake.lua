-- libjhasm：JVM 汇编文本（JAsm）解析/分析/编译/打印库（独立构建根）。
--
-- 依赖：
--   - libjblw（luiox-repo git 直连包）：class 文件运行时面建模。
--   - libjbytecode（luiox-repo git 直连包）：字节码核心面（public 传递）。
--   - libca（luiox-repo git 直连包）：tag_cast 等基础面。
--
-- 仓库注册：本文件不声明 add_repositories——包沙箱会继承宿主的仓库注册，
-- 在工程内重复注册同名仓库会导致嵌套包构建时 remote 路径错乱。独立开发/CI
-- 先执行：
--   xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git

set_languages("cxx17")
add_rules("mode.debug", "mode.release")

add_requires("libjblw 0.0.1")
add_requires("libjbytecode 0.0.2")
add_requires("libca 0.0.15", {configs = {modules = "core"}})

option("with_tests")
    set_default(false)
    set_showmenu(true)
    set_description("Enable unit tests (pulls gtest)")
option_end()

target("libjhasm")
    set_kind("static")
    add_packages("libjblw", "libjbytecode", {public = true})
    add_packages("libca")
    add_includedirs("src", {public = true})
    add_files("src/jhasm/**.cpp")
    add_headerfiles("src/(jhasm/**.hpp)")
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
