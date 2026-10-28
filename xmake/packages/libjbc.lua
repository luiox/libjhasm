-- libjbc 包定义（项目内联定义，源自 luiox-repo 同名定义）
--
-- 为什么项目内联（而不是直接用 luiox-repo 定义）：
-- 1. libjhasm 需要 MethodEditor（从零建方法门面），该面自 v0.4.17 起才有，
--    luiox-repo 的发布版尚未追平（最新 0.4.16）；内联定义锁 commit 直接消费。
-- 2. 上游 0.4.15 定义的 on_load 只传 /std:c++17 不传 /utf-8。libjbc 公开头含
--    UTF-8 中文注释，GBK ACP 的机器上 on_test 探针（has_cxxincludes/
--    check_cxxsnippets，只吃包元数据的 cxxflags）把多行注释误读合并，枚举
--    定义被拆散，报 C2059/C2086 级联，安装直接失败（库本体沙箱构建带工程
--    自己的 /utf-8，不受影响——纯 on_test 面）。这里在 on_load 补 /utf-8
--    （windows），与库自身构建口径一致。项目内联的 package() 定义优先于一切
--    包仓（实测其他目录包仓的同名定义压不过 luiox-repo），是唯一能就地修
--    探针的承载形态。luiox-repo 的 libjbc 定义补齐 0.4.17 与 /utf-8 后，
--    本文件可退役改回仓定义。
--
-- 机制同 libjclassfile：add_urls 指向 git 仓库 + add_versions 第二参数锁
-- commit。xmake 用系统 git 克隆并 checkout 到锁定 commit，随后在本包沙箱内
-- 独立构建安装（不进消费工程的 target 图）。
--
-- 发版/升级：在 libjbc 仓库定版后，于此文件 add_versions 追加新版本条目
-- （既有条目只追加不修改，见 README 可复现纪律）；消费项目自行择机升级。
--
-- 链接说明：整包单库（links = libjbc）。libjbc 工程自身 xmake.lua 已声明
-- libca 依赖（模块 core,str,crypto,zip），包沙箱内自解析；下方 add_deps 是
-- 给消费方的传递声明（另一回事，不可省）。libca 版本/模块钉到 root
-- xmake.lua 的 LUIOX_LIBCA_VERSION/LUIOX_LIBCA_MODULES 单源常量（同一消费图
-- 内各包的 libca (版本, 模块集) 必须完全一致——实例身份 = (版本, configs
-- 哈希)，任何偏差都会多出一个 libca 实例：链接闭包多条 -libpath，同名符号
-- 双归档，解析次序脆弱）。
--
-- 许可：MIT。

package("libjbc")
    set_homepage("https://github.com/luiox/libjbc")
    set_description("node-first JVM bytecode library: lazy classfile DOM with fidelity write-back (git direct package)")
    set_license("MIT")

    add_urls("https://github.com/luiox/libjbc.git")

    -- 语义版本名 + 锁定 commit。版本条目只追加、不修改。
    -- 0.1.0 = 首个独立仓基线（core/ir/analysis/type 四层，MIT 预期未定版）。
    add_versions("0.1.0", "78e8ad8ffd7e8c610e8af1123371c46ee3988318") -- v0.1.0 基线  2026-10-14
    -- 0.4.12 = typed 属性面 13 种 + CP 压缩 + 成员级增删/adopt + 子程序 strict 帧；MIT 定版。
    add_versions("0.4.12", "106cf8302255d18c84b247888724c29bd764a084") -- 重指 GCC 修正版（tag v0.4.12 同 commit；原 a7b12f0 MSVC-only）
    -- 0.4.14 = Nest/Record 属性 typed 建模 + 嵌套属性表 CP 重映射 + nest/record 语料。
    add_versions("0.4.14", "8a00e45613254ba7811b6f196e45afc98c435a9f")
    -- 0.4.15 = Module 族属性 typed 建模 + 压缩封闭集收编；首版测试树完整入库。
    add_versions("0.4.15", "9f1a13ec3df96f3493703ee3c6fbb7c5186187bf")
    -- 0.4.16 = 发版 main 头（luiox-repo 已有同条目，保持一致）。
    add_versions("0.4.16", "edce61d83be812b933731142b811d82c347b3fc0")
    -- 0.4.17 = 移除 consumer 模块（消费者协议面归上层仓）+ MethodEditor
    -- 从零建方法门面 + addMethodWithBody。libjhasm 编译路径依赖 MethodEditor。
    add_versions("0.4.17", "2a44a37010c04fe8772270f019b29422f3ff91b7") -- v0.4.17  2026-10-25
    -- 0.4.18 = MethodParameters 放行接收者行形态（JVMS 4.7.24，javac 对非
    -- 静态方法恒带 this 行）；.parameters 源面依赖本修复。
    add_versions("0.4.18", "29f6ef436461fa48ca27661778ffb8f139633bed") -- v0.4.18  2026-10-26

    -- 0.4.22 = 精确档异常边发射域放宽到范围内可达指令（JVM 校验器保守口径，JVMS 4.10.1.4）：handler 可达 ⟺ 范围内至少一条可达指令，覆盖仅不可抛指令的 try 形态不再缺 handler 帧。
    add_versions("0.4.22", "a408506203aaba7f74cdf588885187d3a49b0bd0") -- PR #19 定版合入头  2026-10-28

    -- 嵌套 libca 直接钉到本仓单源版本（见文件头链接说明）。
    add_deps("libca " .. LUIOX_LIBCA_VERSION, {configs = {modules = LUIOX_LIBCA_MODULES}})

    on_load(function (package)
        package:add("links", "libjbc")
        -- 语言标准传导：公开头使用 C++17（同 libca 包口径）。
        if package:is_plat("windows") then
            package:add("cxxflags", "/std:c++17")
            -- 公开头含 UTF-8 中文注释，消费方编译期（含本包 on_test 探针）必须
            -- 按 UTF-8 读源，GBK ACP 下否则注释吞行致语法错（见文件头说明）。
            package:add("cxxflags", "/utf-8")
        else
            package:add("cxxflags", "-std=c++17")
        end
    end)

    on_install("windows", "linux", "macosx", function (package)
        -- 包安装只装库本体：unittest/demo 均为开发态目标。
        import("package.tools.xmake").install(package, {with_tests = false})
        -- v0.1.0 布局归位：工程 headerfiles 模式 src/jbc/(**.hpp) 的括号组把
        -- jbc/ 前缀剥掉了，沙箱产物是 include/{jbc.hpp,core/,ir/,...} 平铺
        -- 布局，公开头的 #include "jbc/..." 解不到。这里整体搬回 include/jbc/
        -- 下；include/jbc 已存在（上游未来修正了布局）则跳过。附带摘掉误入
        -- 安装面的 test_helper（开发态测试基建，非交付物）。
        local includedir = path.join(package:installdir(), "include")
        if not os.isdir(path.join(includedir, "jbc")) then
            local staged = path.join(includedir, "jbc")
            os.mkdir(staged)
            for _, entry in ipairs({"jbc.hpp", "analysis", "core", "ir", "type"}) do
                os.mv(path.join(includedir, entry), path.join(staged, entry))
            end
        end
        os.tryrm(path.join(package:installdir(), "lib", "test_helper.lib"))
        os.tryrm(path.join(package:installdir(), "lib", "libtest_helper.a"))
    end)

    on_test(function (package)
        assert(package:has_cxxincludes("jbc/jbc.hpp", {configs = {languages = "cxx17"}}))
        -- 链接级自检：get_lib_version 强制解析 libjbc + libca 的符号链
        -- （缺任一环即在此暴露）。
        assert(package:check_cxxsnippets({test = [[
            #include "jbc/jbc.hpp"
            int main(int argc, char** argv) {
                return jbc::get_lib_version() != nullptr && argc >= 0 ? 0 : 1;
            }
        ]]}, {configs = {languages = "cxx17"}}))
    end)
package_end()
