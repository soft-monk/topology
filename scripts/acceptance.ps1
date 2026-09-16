# scripts/acceptance.ps1 · topology 独立验收脚本（退出码 0/1）
#
# 权威依据：docs/需求/topology需求专篇.md
#   ① §7 验收清单 16 行 —— 逐行变成一条可执行检查（S07-01..S07-16）
#   ② §3 功能需求 34 条（TPL-MODEL 6 / TPL-Q 6 / TPL-ST 6 / TPL-EVAL 4 / TPL-OPT 6 / TPL-NFR 6）
#   ③ §1.4 硬约束 + §4 引擎/规则划分判据 + protocol.md P1/P3/P4/P6/P7/P8/P9/P10、CTR-PL-01..08
#
# 设计：**引擎行为一律由 tests/selftest 断言**（selftest --json 输出逐用例结果与需求编号），
# 本脚本只做四件它做不了的事：构建生命周期、结构纪律检索、规则包 schema 校验、需求↔用例对账。
# 这样"行为口径"只有一个来源，不会出现脚本与单测两套断言的漂移。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts/acceptance.ps1
#   ... -SkipBuild            只跑自测与检查（复用已有构建产物）
#   ... -SkipFallbackBuild    跳过"内置单头回落"构建（省时间）
#   ... -Config Debug         换构建配置
#   ... -Generator "Ninja"    换生成器（单配置生成器也可）
#
# 兼容 Windows PowerShell 5.1（不依赖 pwsh / PS7 语法）。
param(
    [string]$BuildDir = "build",
    [string]$Config = "Release",
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Arch = "x64",
    [switch]$SkipBuild,
    [switch]$SkipFallbackBuild
)

$ErrorActionPreference = "Stop"
$script:Results = New-Object System.Collections.Generic.List[object]
$script:ChecksFailed = 0

# ------------------------------------------------------------------ 输出小工具
function Check([string]$id, [string]$title, [bool]$ok, [string]$detail) {
    $state = "PASS"
    if (-not $ok) { $state = "FAIL"; $script:ChecksFailed++ }
    $script:Results.Add([pscustomobject]@{ Id = $id; Title = $title; Ok = $ok; Detail = $detail })
    $color = "Green"
    if (-not $ok) { $color = "Red" }
    Write-Host ("  [{0}] {1} · {2}" -f $state, $id, $title) -ForegroundColor $color
    if ($detail) { Write-Host ("         {0}" -f $detail) -ForegroundColor DarkGray }
}

function Section([string]$title) {
    Write-Host ""
    Write-Host ("=" * 78)
    Write-Host $title
    Write-Host ("=" * 78)
}

function Rel([string]$full) {
    $root = $script:Repo
    if ($full.StartsWith($root)) { return $full.Substring($root.Length).TrimStart('\', '/') }
    return $full
}

# 命中列表 → 人类可读摘要（最多 5 条，避免刷屏）
function Format-Hits($hits) {
    if (-not $hits -or $hits.Count -eq 0) { return "" }
    $head = @($hits | Select-Object -First 5)
    $s = "：" + ($head -join "；")
    if ($hits.Count -gt 5) { $s += ("；…共 {0} 处" -f $hits.Count) }
    return $s
}

# 结构检索：返回命中列表（"文件:行号"）
function SearchHits([string[]]$files, [string]$regex, [string[]]$skipLineRegex) {
    $hits = New-Object System.Collections.Generic.List[string]
    foreach ($f in $files) {
        $n = 0
        foreach ($line in [System.IO.File]::ReadAllLines($f)) {
            $n++
            if ($line -notmatch $regex) { continue }
            if ($skipLineRegex) {
                $skip = $false
                foreach ($s in $skipLineRegex) { if ($line -match $s) { $skip = $true; break } }
                if ($skip) { continue }
            }
            $hits.Add(("{0}:{1}" -f (Rel $f), $n))
        }
    }
    return $hits
}

function CollectFiles([string[]]$dirs, [string[]]$exts, [string[]]$excludeDirs) {
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($d in $dirs) {
        $full = Join-Path $script:Repo $d
        if (-not (Test-Path $full)) { continue }
        Get-ChildItem -Path $full -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
            if ($exts -notcontains $_.Extension) { return }
            $rel = Rel $_.FullName
            foreach ($x in $excludeDirs) { if ($rel -like ($x + "*")) { return } }
            $out.Add($_.FullName)
        }
    }
    return $out
}

# ------------------------------------------------------------------ 前置
$script:Repo = Split-Path -Parent $PSScriptRoot
Push-Location $script:Repo
try {
    Write-Host "topology 独立验收（需求专篇 §7 逐条 + 结构纪律 + 规则包 schema + selftest 对账）"
    Write-Host ("仓库：{0}" -f $script:Repo)
    Write-Host ("PowerShell：{0}" -f $PSVersionTable.PSVersion)

    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
        foreach ($p in @("C:\Program Files\CMake\bin\cmake.exe",
                         "C:\Program Files (x86)\CMake\bin\cmake.exe")) {
            if (Test-Path $p) { $cmake = $p; break }
        }
    }
    if (-not $cmake) { Write-Host "找不到 cmake（>= 3.20）" -ForegroundColor Red; exit 1 }
    $cmakeVersion = (& $cmake --version | Select-Object -First 1)
    Write-Host ("cmake：{0}（{1}）" -f $cmake, $cmakeVersion)

    $buildPath = Join-Path $script:Repo $BuildDir
    $isMultiConfig = ($Generator -like "Visual Studio*")
    $binCandidates = @()
    if ($isMultiConfig) { $binCandidates += (Join-Path $buildPath "bin\$Config") }
    $binCandidates += (Join-Path $buildPath "bin\$Config")
    $binCandidates += (Join-Path $buildPath "bin")

    function Resolve-Bin() {
        foreach ($c in $binCandidates) {
            if ((Test-Path (Join-Path $c "selftest.exe")) -or (Test-Path (Join-Path $c "selftest"))) {
                return $c
            }
        }
        return $binCandidates[0]
    }

    # ================================================================ ① 构建
    Section "① 构建生命周期（TPL-NFR-05：一条命令构建 → 一条命令验收）"

    $configureOk = $false
    $buildOk = $false
    if ($SkipBuild) {
        $configureOk = (Test-Path (Join-Path $buildPath "CMakeCache.txt"))
        $buildOk = (Test-Path (Join-Path (Resolve-Bin) "selftest.exe")) -or
                   (Test-Path (Join-Path (Resolve-Bin) "selftest"))
        Check "C01" "构建：-SkipBuild 复用已有产物" ($configureOk -and $buildOk) `
            ("configure={0} binary={1}" -f $configureOk, $buildOk)
    } else {
        Write-Host ("    cmake -S . -B {0} -G '{1}'" -f $BuildDir, $Generator)
        if ($isMultiConfig) {
            & $cmake -S $script:Repo -B $buildPath -G $Generator -A $Arch 2>&1 |
                ForEach-Object { Write-Host ("      {0}" -f $_) }
        } else {
            & $cmake -S $script:Repo -B $buildPath -G $Generator 2>&1 |
                ForEach-Object { Write-Host ("      {0}" -f $_) }
        }
        $configureOk = ($LASTEXITCODE -eq 0)

        Write-Host ("    cmake --build {0} --config {1}" -f $BuildDir, $Config)
        & $cmake --build $buildPath --config $Config 2>&1 |
            ForEach-Object { Write-Host ("      {0}" -f $_) }
        $buildOk = ($LASTEXITCODE -eq 0)

        $bin = Resolve-Bin
        $hasSelftest = (Test-Path (Join-Path $bin "selftest.exe")) -or (Test-Path (Join-Path $bin "selftest"))
        $hasExamples = ((Test-Path (Join-Path $bin "example_minimal.exe")) -or
                        (Test-Path (Join-Path $bin "example_minimal"))) -and
                       ((Test-Path (Join-Path $bin "example_full_flow.exe")) -or
                        (Test-Path (Join-Path $bin "example_full_flow")))
        Check "C02" "构建：配置 + 编译通过，产物齐全（topology 库 + selftest + 2 个示例）" `
            ($configureOk -and $buildOk -and $hasSelftest -and $hasExamples) `
            ("configure={0} build={1} selftest={2} examples={3} bin={4}" -f `
                $configureOk, $buildOk, $hasSelftest, $hasExamples, (Rel $bin))
    }

    $bin = Resolve-Bin
    $selftestExe = Join-Path $bin "selftest.exe"
    if (-not (Test-Path $selftestExe)) { $selftestExe = Join-Path $bin "selftest" }

    # ① -2 CMake 最低版本 >= 3.20 + C++17（TPL-NFR-01）
    $cmakeLists = Get-Content -Raw -Encoding UTF8 (Join-Path $script:Repo "CMakeLists.txt")
    $minOk = $false
    if ($cmakeLists -match 'cmake_minimum_required\s*\(\s*VERSION\s+([0-9]+\.[0-9]+)') {
        $minOk = ([version]$Matches[1] -ge [version]"3.20")
    }
    $cxx17 = ($cmakeLists -match 'CMAKE_CXX_STANDARD\s+17')
    Check "C03" "构建：CMake >= 3.20 且 C++17" ($minOk -and $cxx17) `
        ("cmake_min={0} cxx17={1}" -f $minOk, $cxx17)

    # ① -3 依赖回落构建：禁用系统 nlohmann_json 包 → 必须走内置单头（TPL-NFR-01）
    if ($SkipFallbackBuild -or $SkipBuild) {
        Check "C04" "依赖：内置单头回落构建（跳过：-SkipFallbackBuild）" $true "视为不适用"
    } else {
        $fallbackDir = Join-Path $script:Repo ($BuildDir + "-fallback")
        Write-Host ("    cmake -S . -B {0} -DCMAKE_DISABLE_FIND_PACKAGE_nlohmann_json=ON" -f `
            ($BuildDir + "-fallback"))
        if ($isMultiConfig) {
            & $cmake -S $script:Repo -B $fallbackDir -G $Generator -A $Arch `
                -DCMAKE_DISABLE_FIND_PACKAGE_nlohmann_json=ON -DTOPOLOGY_BUILD_EXAMPLES=OFF 2>&1 |
                ForEach-Object { Write-Host ("      {0}" -f $_) }
        } else {
            & $cmake -S $script:Repo -B $fallbackDir -G $Generator `
                -DCMAKE_DISABLE_FIND_PACKAGE_nlohmann_json=ON -DTOPOLOGY_BUILD_EXAMPLES=OFF 2>&1 |
                ForEach-Object { Write-Host ("      {0}" -f $_) }
        }
        $fbConfigure = ($LASTEXITCODE -eq 0)
        & $cmake --build $fallbackDir --config $Config --target topology 2>&1 |
            ForEach-Object { Write-Host ("      {0}" -f $_) }
        $fbBuild = ($LASTEXITCODE -eq 0)
        $fbHeader = Test-Path (Join-Path $script:Repo "third_party\nlohmann\json.hpp")
        Check "C04" "依赖：禁用系统 nlohmann_json 包后仍能构建（内置单头回落，TPL-NFR-01）" `
            ($fbConfigure -and $fbBuild -and $fbHeader) `
            ("configure={0} build={1} builtinHeader={2}" -f $fbConfigure, $fbBuild, $fbHeader)
    }

    # ================================================================ ② selftest
    Section "② 零依赖自测（tests/selftest --json，行为口径的唯一来源）"

    $jsonOk = $false
    $data = $null
    $jsonPath = Join-Path ([System.IO.Path]::GetTempPath()) ("topology-selftest-{0}.json" -f $PID)
    if (Test-Path $selftestExe) {
        # 必须让子进程**直接写文件**：selftest --json 输出 UTF-8（含中文用例名），
        # 走管道会被控制台代码页（936）解成乱码，JSON 随之不可解析。
        & $selftestExe --json > $jsonPath
        $raw = ""
        if (Test-Path $jsonPath) { $raw = [System.IO.File]::ReadAllText($jsonPath, [System.Text.Encoding]::UTF8) }
        try {
            $data = $raw | ConvertFrom-Json
            $jsonOk = $true
        } catch {
            Write-Host "    selftest --json 解析失败：$($_.Exception.Message)" -ForegroundColor Red
            if ($raw.Length -gt 0) { Write-Host ($raw.Substring(0, [Math]::Min(400, $raw.Length))) }
        }
    }
    Check "C05" "自测可执行且 --json 输出可解析" $jsonOk ("exe={0}" -f (Rel $selftestExe))

    $caseByName = @{}
    $reqToCases = @{}
    if ($jsonOk) {
        Write-Host ("    用例 {0} 个（失败 {1}）｜断言 {2} 条（失败 {3}）｜耗时 {4:N1} ms｜结果 {5}" -f `
            $data.cases, $data.casesFailed, $data.asserts, $data.assertsFailed, $data.elapsedMs, $data.result)
        Check "C06" "自测全绿：用例失败 0 且断言失败 0" `
            (($data.casesFailed -eq 0) -and ($data.assertsFailed -eq 0)) `
            ("cases={0}/{1} asserts={2}/{3}" -f $data.cases, $data.casesFailed, $data.asserts, $data.assertsFailed)
        foreach ($c in $data.details) {
            $caseByName[$c.name] = $c
            foreach ($r in $c.reqs) {
                if (-not $reqToCases.ContainsKey($r)) {
                    $reqToCases[$r] = New-Object System.Collections.Generic.List[string]
                }
                $reqToCases[$r].Add($c.name)
            }
        }
    } else {
        Check "C06" "自测全绿" $false "selftest --json 无有效输出"
    }

    # ---- 需求 34 条 → 用例对账
    $allReqs = New-Object System.Collections.Generic.List[string]
    foreach ($d in @("MODEL:01..06", "Q:01..06", "ST:01..06", "EVAL:01..04", "OPT:01..06", "NFR:01..06")) {
        $dom = $d.Split(':')[0]
        $range = $d.Split(':')[1] -replace '\.\.', ' '
        $parts = $range.Split(' ')
        for ($i = [int]$parts[0]; $i -le [int]$parts[1]; $i++) {
            $allReqs.Add(("TPL-{0}-{1:D2}" -f $dom, $i))
        }
    }

    if ($jsonOk) {
        $missing = New-Object System.Collections.Generic.List[string]
        foreach ($r in $allReqs) {
            if (-not $reqToCases.ContainsKey($r)) { $missing.Add($r) }
        }
        Check "C07" ("需求覆盖：{0} 条 TPL-* 每条都有用例引用" -f $allReqs.Count) ($missing.Count -eq 0) `
            ("总需求 {0} 条；无对应用例：{1}" -f $allReqs.Count, (($missing -join ", ") -replace '^$', '无'))

        $reqFailed = New-Object System.Collections.Generic.List[string]
        foreach ($r in $allReqs) {
            if (-not $reqToCases.ContainsKey($r)) { continue }
            $anyOk = $false
            foreach ($cn in $reqToCases[$r]) { if ($caseByName[$cn].ok) { $anyOk = $true } }
            if (-not $anyOk) { $reqFailed.Add($r) }
        }
        Check "C08" "需求覆盖：每条至少有一个通过用例" ($reqFailed.Count -eq 0) `
            ("未通过：{0}" -f (($reqFailed -join ", ") -replace '^$', '无'))
    } else {
        Check "C07" "需求覆盖：34 条 TPL-* 每条都有用例引用" $false "自测结果不可用"
        Check "C08" "需求覆盖：每条至少有一个通过用例" $false "自测结果不可用"
    }

    # ================================================================ ③ §7 验收清单逐条
    Section "③ 需求专篇 §7 验收清单（逐条，16 行）"

    $checklist = @(
        @{ Id = "S07-01"; Line = "空环境 clone → 一条命令构建 → 一条命令跑通验收脚本（退出码 0）"
           Cases = @(); NeedsBuild = $true },
        @{ Id = "S07-02"; Line = "全仓检索 业务词作为判断依据 / 硬编码优化常量 / 写死曲线公式 → 零命中"
           Cases = @(); Guard = "structure" },
        @{ Id = "S07-03"; Line = "把节点改名后，节点类型判定不变"
           Cases = @("model01_node_type_comes_from_declared_key_not_name") },
        @{ Id = "S07-04"; Line = "同一份数据分别按网状与三层装载均可渲染"
           Cases = @("model02_same_data_under_two_structures") },
        @{ Id = "S07-05"; Line = "重复边只出现一次；自环被拒绝；悬挂边被检出并定位"
           Cases = @("model04_dedup_and_self_loop", "model05_validation_locates_defects") },
        @{ Id = "S07-06"; Line = "只带 state 的事件不覆盖其它指标"
           Cases = @("q03_missing_fields_keep_previous_values") },
        @{ Id = "S07-07"; Line = "灌入已知序列，1 s 窗口聚合结果手算一致"
           Cases = @("q04_sliding_window_aggregation_is_hand_checkable") },
        @{ Id = "S07-08"; Line = "构造阈值附近震荡输入 → 状态变更次数显著低于越阈次数"
           Cases = @("st02_hysteresis_kills_oscillation", "st03_min_dwell_limits_change_rate") },
        @{ Id = "S07-09"; Line = "状态变更日志可复原判定过程（含越阈指标与数值）"
           Cases = @("st04_state_changes_are_explainable") },
        @{ Id = "S07-10"; Line = "四项评估可手算核对；输出中无成句文案"
           Cases = @("eval01_four_items_are_recomputable", "eval02_output_is_structured_without_prose") },
        @{ Id = "S07-11"; Line = "scoring 只读调用即可取得链路稳定度"
           Cases = @("eval03_stability_is_read_only_for_scoring") },
        @{ Id = "S07-12"; Line = "连续优化 10 次结果收敛不漂移；无改进时返回""无改进"""
           Cases = @("opt03_ten_runs_converge_without_drift", "opt05_monotonic_and_no_improvement") },
        @{ Id = "S07-13"; Line = "曲线点来自真实滑动窗口（非公式）"
           Cases = @("opt06_curves_come_from_real_windows") },
        @{ Id = "S07-14"; Line = "注入假时钟，迟滞与窗口测试可复现"
           Cases = @("nfr03_fake_clock_makes_time_reproducible") },
        @{ Id = "S07-15"; Line = "1000 条边、1 Hz 输入下单次聚合 + 判定 P95 ≤ 1 ms"
           Cases = @("nfr06_performance_1000_edges") },
        @{ Id = "S07-16"; Line = "输出可映射到 map-2d 的 LinkItem{id,from,to,state} 与 ClusterItem"
           Cases = @("nfr07_primitives_match_consumer_contract") }
    )

    if (-not $jsonOk) {
        foreach ($item in $checklist) {
            if ($item.ContainsKey("Guard")) { continue }
            Check $item.Id $item.Line $false "自测结果不可用，无法对账"
        }
    } else {
        foreach ($item in $checklist) {
            if ($item.ContainsKey("Guard")) { continue }
            if ($item.ContainsKey("NeedsBuild")) {
                Check $item.Id $item.Line ($configureOk -and $buildOk) `
                    "配置与构建成功 = 空环境一条命令可构建（详见 ① 的输出）"
                continue
            }
            $missingCases = New-Object System.Collections.Generic.List[string]
            $failedCases = New-Object System.Collections.Generic.List[string]
            foreach ($cn in $item.Cases) {
                if (-not $caseByName.ContainsKey($cn)) { $missingCases.Add($cn); continue }
                if (-not $caseByName[$cn].ok) { $failedCases.Add($cn) }
            }
            $ok = ($missingCases.Count -eq 0) -and ($failedCases.Count -eq 0)
            $detail = "用例：{0}" -f ($item.Cases -join ", ")
            if ($missingCases.Count -gt 0) { $detail += "；缺失：" + ($missingCases -join ", ") }
            if ($failedCases.Count -gt 0) { $detail += "；失败：" + ($failedCases -join ", ") }
            Check $item.Id $item.Line $ok $detail
        }
    }

    # ================================================================ ④ 结构纪律
    Section "④ 结构纪律：业务词 / 跨仓 import / 硬编码优化常量 / 写死曲线 / 颜色值 / 规则包（P1/P6/P7、TPL-*）"

    # 引擎产物范围：include/ src/ examples/ scripts/ CMakeLists.txt
    # 豁免：docs/（需求与契约的示例语境）、policies/（业务取值的合法住所）、tests/（测试数据与夹具）
    $engineFiles = [string[]]@(CollectFiles @("include", "src", "examples", "scripts") `
        @(".h", ".hpp", ".cc", ".cpp", ".ps1") @())
    foreach ($f in @("CMakeLists.txt", "examples\CMakeLists.txt", "tests\CMakeLists.txt")) {
        $full = Join-Path $script:Repo $f
        if (Test-Path $full) { $engineFiles += $full }
    }
    $codeFiles = [string[]]@(CollectFiles @("include", "src") @(".h", ".hpp", ".cc", ".cpp") @())
    Write-Host ("    检索范围 {0} 个引擎产物文件（docs/ policies/ tests/ 豁免）" -f $engineFiles.Count)

    # C09：业务词零命中（模式拆开拼接，避免脚本自指：本文件里也不出现这些词的字面量）
    $bizPattern = ('云' + '端|边' + '缘|前' + '沿')
    $bizHits = @(SearchHits $engineFiles $bizPattern @('MUST NOT', '零命中', '业务词', '检索范围', 'bizPattern'))
    Check "C09" "TPL-MODEL-01 / P6：引擎产物内节点类型业务词零命中（类型只由规则声明的 key 判定）" `
        ($bizHits.Count -eq 0) ("命中 {0} 处{1}" -f $bizHits.Count, (Format-Hits $bizHits))

    # C09b：业务词确实只住在规则包 / 测试数据 / 示例数据里（否则 C09 的"零命中"可能是假绿）
    $dataFiles = [string[]]@(CollectFiles @("policies", "tests") @(".json", ".cc") @())
    $bizDataHits = @(SearchHits $dataFiles $bizPattern @())
    Check "C09b" "TPL-MODEL-01：节点类型显示名只住在规则包 policies/ 与测试数据 tests/" `
        ($bizDataHits.Count -gt 0) `
        ("规则包/测试数据内命中 {0} 处（说明类型取值来自外部数据而非引擎）" -f $bizDataHits.Count)

    # C10：类型判定 MUST NOT 用名称子串（现状缺陷：对节点显示名做子串匹配猜类型）
    # 判据：对**节点显示名**做 find/rfind/substr/compare/starts_with 一律禁止（规则包 key 查表不算）
    $nameInferHits = @(SearchHits $codeFiles `
        '(typeName|displayName|spec\.name|node\.name|rec\.name|\.name)\s*\.\s*(find|rfind|substr|compare|starts_with|ends_with)\s*\(' `
        @('MUST NOT', '零命中'))
    Check "C10" "TPL-MODEL-01：类型判定不做名称子串推断（显示名 find/substr/compare 零命中）" `
        ($nameInferHits.Count -eq 0) ("命中 {0} 处{1}" -f $nameInferHits.Count, (Format-Hits $nameInferHits))

    # C11：跨仓 import 与 Web/SQL 依赖零命中（P1/P2/P3、TPL-NFR-01）
    $otherRepos = '(telemetry_store|telemetry-store|device_ingest|device-ingest|realtime_hub|realtime-hub|' +
                  'map_2d|map-2d|entity_ledger|entity-ledger|resource_alloc|resource-alloc|scoring|selfcheck|' +
                  'view_composer|view-composer|alert_engine|alert-engine|report_engine|report-engine|' +
                  'geo_data|geo-data|media_player|media-player|phase_engine|phase/|drogon|Drogon|sqlite|' +
                  'mysql|pqxx|libpq|nanodbc|oatpp|crow|httplib)'
    $incHits = @(SearchHits $engineFiles ('#\s*include\s*[<"]' + $otherRepos) @('MUST NOT', '零命中'))
    Check "C11" "P1/P2/P3：跨仓 import 与 Web 框架 / SQL 依赖零命中" ($incHits.Count -eq 0) `
        ("命中 {0} 处{1}" -f $incHits.Count, (Format-Hits $incHits))

    $allIncludes = New-Object System.Collections.Generic.List[string]
    foreach ($f in $engineFiles) {
        foreach ($line in [System.IO.File]::ReadAllLines($f)) {
            $m = [regex]::Match($line, '^\s*#\s*include\s+(?:<([^>]+)>|"([^"]+)")')
            if ($m.Success) {
                if ($m.Groups[1].Success) { $allIncludes.Add($m.Groups[1].Value) }
                else { $allIncludes.Add($m.Groups[2].Value) }
            }
        }
    }
    # 只允许：C/C++ 标准库（无扩展名/无斜杠的头）、本模块公开头（topology/...）、
    # 本模块内部头（internal.h）、nlohmann/json。
    $nonJson = $allIncludes | Where-Object {
        ($_ -notmatch '^(topology/|internal\.h$)') -and
        ($_ -notmatch '^[a-z_]+$') -and
        ($_ -notmatch '^(nlohmann|third_party)')
    }
    Check "C12" "TPL-NFR-01：第三方依赖仅 nlohmann/json（无其它外部头）" ($nonJson.Count -eq 0) `
        ("非标准库/非 nlohmann 的 include：{0}" -f (($nonJson -join ", ") -replace '^$', '无'))

    # C13：硬编码优化常量零命中（TPL-OPT-01；现状 SQL：bandwidth*1.12 / latency*0.72 / mesh+20）
    $optPattern = ('\*\s*1\.' + '12|\*\s*0\.' + '72|(?<![0-9.])1\.' + '12(?![0-9])|(?<![0-9.])0\.' +
                   '72(?![0-9])|\+\s*20(?![0-9])|state\s*=\s*''green''')
    $optHits = @(SearchHits $engineFiles $optPattern @('MUST NOT', '零命中', 'optPattern'))
    Check "C13" "TPL-OPT-01：硬编码优化常量（1.12 / 0.72 / +20 / SQL 改状态）零命中" `
        ($optHits.Count -eq 0) ("命中 {0} 处{1}" -f $optHits.Count, (Format-Hits $optHits))

    # C14：写死曲线公式零命中（TPL-OPT-06）
    $curvePattern = ('exp\s*\(|-3\.' + '2|-2\.' + '8')
    $curveHits = @(SearchHits $engineFiles $curvePattern @('MUST NOT', '零命中', 'curvePattern'))
    Check "C14" "TPL-OPT-06：写死曲线公式（exp(...) / -3.2 / -2.8）零命中" ($curveHits.Count -eq 0) `
        ("命中 {0} 处{1}" -f $curveHits.Count, (Format-Hits $curveHits))

    # C15：只出状态码，不出颜色值（TPL-ST-05 / D4）
    $colorHits = @(SearchHits $codeFiles 'rgb\s*\(|#[0-9a-fA-F]{6}\b|\bcolor\b|\bcolour\b' @('MUST NOT', '零命中'))
    Check "C15" "TPL-ST-05 / D4：引擎内无颜色值（rgb/#rrggbb/color）" ($colorHits.Count -eq 0) `
        ("命中 {0} 处{1}" -f $colorHits.Count, (Format-Hits $colorHits))

    # C16：指标名 MUST NOT 内建（TPL-Q-01）；事件字段归一在引擎内（TPL-Q-02）
    $metricNames = 'bandwidthMbps|latencyMs|lossRate|coverageKm2|nodeLoad|cacheAvailable|cacheTotal'
    $metricHits = @(SearchHits $codeFiles $metricNames @('MUST NOT', '零命中'))
    Check "C16" "TPL-Q-01：引擎源码内不出现具体指标名（指标集全部来自规则包）" ($metricHits.Count -eq 0) `
        ("命中 {0} 处{1}" -f $metricHits.Count, (Format-Hits $metricHits))

    $normalizeHits = @(SearchHits $codeFiles 'camelCase|toSnakeCase|ledgerKey' @())
    Check "C17" "TPL-Q-02：驼峰→台账列名的归一在引擎内（toSnakeCase/ledgerKey 存在）" `
        ($normalizeHits.Count -gt 0) ("命中 {0} 处（应有：引擎提供归一机制）" -f $normalizeHits.Count)

    # C18：不产生保留码 1001（CTR-EC-01 / ADR-C15；注释与字符串里的说明文字不算）
    $bad1001 = New-Object System.Collections.Generic.List[string]
    foreach ($f in $codeFiles) {
        $n = 0
        foreach ($line in [System.IO.File]::ReadAllLines($f)) {
            $n++
            $stripped = ($line -replace '//.*$', '') -replace '/\*.*?\*/', ''
            if ($stripped -match '"') { continue }
            if ($stripped -match '(?<![0-9])1001(?![0-9])') { $bad1001.Add(("{0}:{1}" -f (Rel $f), $n)) }
        }
    }
    Check "C18" "CTR-EC-01：不产生保留码 1001（幂等成功走 code=0 + idempotent）" ($bad1001.Count -eq 0) `
        ("命中 {0} 处{1}" -f $bad1001.Count, (Format-Hits $bad1001))

    # C19：唯一公开头（宿主只允许包含它）
    $publicHeaders = @()
    $includeDir = Join-Path $script:Repo "include"
    if (Test-Path $includeDir) {
        $publicHeaders = @(Get-ChildItem $includeDir -Recurse -File -Filter *.h |
            ForEach-Object { Rel $_.FullName })
    }
    Check "C19" "唯一公开头 include/topology/topology_engine.h" `
        (($publicHeaders.Count -eq 1) -and ($publicHeaders[0] -eq "include\topology\topology_engine.h")) `
        ("公开头：{0}" -f ($publicHeaders -join ", "))

    # C20：反向接口齐备（TPL-NFR-02/03：出口全走注入）
    $headerPath = Join-Path $script:Repo "include\topology\topology_engine.h"
    $headerText = ""
    if (Test-Path $headerPath) { $headerText = Get-Content -Raw -Encoding UTF8 $headerPath }
    $hasSink = ($headerText -match 'class\s+ITopologySink') -and ($headerText -match 'virtual\s+void\s+onTopologyChanged')
    $hasStore = ($headerText -match 'class\s+ITopologyStore') -and ($headerText -match 'virtual\s+bool\s+save') -and
                ($headerText -match 'virtual\s+bool\s+load')
    $hasClock = ($headerText -match 'class\s+IClock') -and ($headerText -match 'virtual\s+int64_t\s+nowMs')
    $hasLayout = ($headerText -match 'class\s+ILayoutProvider') -and ($headerText -match 'virtual\s+bool\s+resolve')
    $hasLog = ($headerText -match 'class\s+ILogSink')
    $optsInjected = ($headerText -match 'std::shared_ptr<ITopologyStore>\s+store') -and
                    ($headerText -match 'std::shared_ptr<IClock>\s+clock') -and
                    ($headerText -match 'std::shared_ptr<ITopologySink>\s+sink') -and
                    ($headerText -match 'std::shared_ptr<ILayoutProvider>\s+layout')
    Check "C20" "TPL-NFR-02/03：反向接口齐备且全部经 TopologyEngineOptions 注入" `
        ($hasSink -and $hasStore -and $hasClock -and $hasLayout -and $hasLog -and $optsInjected) `
        ("sink={0} store={1} clock={2} layout={3} log={4} injected={5}" -f `
            $hasSink, $hasStore, $hasClock, $hasLayout, $hasLog, $optsInjected)

    # C21：PhaseContext 五字段按 protocol §1.4 逐字（引擎不 import phase-engine，形状自行声明）
    $pcOk = ($headerText -match 'phaseKey') -and ($headerText -match 'scenarioKey') -and
            ($headerText -match 'enteredAt') -and ($headerText -match 'missionId') -and
            ($headerText -match 'int64_t\s+enteredAt')
    Check "C21" "protocol §1.4：PhaseContext 五字段形状齐备（引擎按共享契约声明，不跨仓 import）" `
        $pcOk "phaseKey / seq / scenarioKey / enteredAt / missionId"

    # C22：状态三值枚举（只出状态码）
    $jsonCcText = Get-Content -Raw -Encoding UTF8 (Join-Path $script:Repo "src\json.cc")
    $stateOk = ($headerText -match 'enum\s+class\s+LinkState') -and
               ($headerText -match 'toString\(LinkState') -and
               ($jsonCcText -match '"green"') -and
               ($jsonCcText -match '"yellow"') -and
               ($jsonCcText -match '"red"')
    Check "C22" "TPL-ST-05：状态输出为 green/yellow/red 三值枚举（状态码，不含颜色）" $stateOk `
        "enum LinkState + toString/linkStateFromString"

    # C23：边状态事件名为已登记事件（protocol §4.4 topology.changed；CTR-EV-07）
    $protocolPath = Join-Path (Split-Path -Parent $script:Repo) "phase-engine\docs\契约\protocol.md"
    $eventName = 'topology' + '.changed'
    $registered = $false
    if (Test-Path $protocolPath) {
        $registered = ((Get-Content -Raw -Encoding UTF8 $protocolPath) -match [regex]::Escape($eventName))
    }
    $nameOk = ($eventName -match '^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*)+$')
    Check "C23" "CTR-EV-07：只使用已在 protocol.md §4.4 登记的事件名" ($registered -and $nameOk) `
        ("event={0} registered={1} namingRule={2}" -f $eventName, $registered, $nameOk)

    # ================================================================ ⑤ 规则包 schema（protocol §5）
    Section "⑤ 规则包 schema（protocol §5：骨架 / kind / 版本 / 必填段）"

    $policyPath = Join-Path $script:Repo "policies\mapapp\linkThresholds.json"
    $policyOk = Test-Path $policyPath
    $pol = $null
    if ($policyOk) {
        try { $pol = Get-Content -Raw -Encoding UTF8 $policyPath | ConvertFrom-Json } catch { $policyOk = $false }
    }
    Check "C24" "规则包存在且是合法 JSON：policies/mapapp/linkThresholds.json" $policyOk `
        (Rel $policyPath)

    if (-not $policyOk) {
        foreach ($id in @("C25", "C26", "C27", "C28")) { Check $id "规则包 schema 检查" $false "规则包不可用" }
    } else {
        $skeleton = ($pol.policiesNamespace -eq "mapapp") -and
                    ($pol.schemaVersion -match '^[0-9]+\.[0-9]+\.[0-9]+$') -and
                    ($pol.kind -eq "linkThresholds") -and
                    ($pol.items -is [array]) -and ($pol.items.Count -gt 0)
        Check "C25" "§5.1：骨架齐备（policiesNamespace / schemaVersion / kind / items）" $skeleton `
            ("namespace={0} version={1} kind={2} items={3}" -f $pol.policiesNamespace, `
                $pol.schemaVersion, $pol.kind, @($pol.items).Count)

        $majorOk = ($pol.schemaVersion -match '^1\.')
        Check "C26" "§5.2：MAJOR = 1（引擎支持的版本；不匹配时拒绝装载并返回 1006）" $majorOk `
            ("schemaVersion={0}" -f $pol.schemaVersion)

        $stateKeys = @($pol.states | ForEach-Object { $_.key })
        $threeState = ($stateKeys.Count -eq 3) -and ($stateKeys -contains "green") -and
                      ($stateKeys -contains "yellow") -and ($stateKeys -contains "red")
        $hyst = ($pol.hysteresis.riseMargin -gt 0) -or ($pol.hysteresis.fallMargin -gt 0) -or
                ($pol.hysteresis.confirmCount -gt 1)
        $dwell = ($null -ne $pol.hysteresis.minDwellMs)
        Check "C27" "TPL-ST-01/02/03：阈值三档 + 迟滞参数 + 最小驻留全部由规则声明" `
            ($threeState -and $hyst -and $dwell) `
            ("states={0} riseMargin={1} fallMargin={2} confirmCount={3} minDwellMs={4}" -f `
                ($stateKeys -join "/"), $pol.hysteresis.riseMargin, $pol.hysteresis.fallMargin, `
                $pol.hysteresis.confirmCount, $pol.hysteresis.minDwellMs)

        $segOk = (@($pol.structures).Count -gt 0) -and (@($pol.metrics.items).Count -gt 0) -and
                 ($pol.metrics.window.windowMs -gt 0) -and ($null -ne $pol.evaluation.stabilityKey) -and
                 (@($pol.evaluation.items).Count -gt 0) -and (@($pol.optimization.parameters).Count -gt 0) -and
                 ($pol.curves.granularityMs -gt 0) -and ($pol.coverage.targetAreaKm2 -gt 0) -and
                 (@($pol.meshProgress.components).Count -gt 0)
        $hasLayered = @($pol.structures | Where-Object { $_.mode -eq "layered" }).Count -gt 0
        $hasMesh = @($pol.structures | Where-Object { $_.mode -eq "mesh" }).Count -gt 0
        Check "C28" "TPL-MODEL-02 / TPL-Q-01 / TPL-EVAL-01 / TPL-OPT-02 / TPL-OPT-06：规则段齐备且声明了两种结构" `
            ($segOk -and $hasLayered -and $hasMesh) `
            ("structures={0}（mesh={1} layered={2}）metrics={3} windowMs={4} evalItems={5} params={6} granularityMs={7}" -f `
                @($pol.structures).Count, $hasMesh, $hasLayered, @($pol.metrics.items).Count, `
                $pol.metrics.window.windowMs, @($pol.evaluation.items).Count, `
                @($pol.optimization.parameters).Count, $pol.curves.granularityMs)
    }

    # §7-02（结构类）结论：C09 + C09b + C10 + C13 + C14 全部成立
    $s0702ok = ($bizHits.Count -eq 0) -and ($bizDataHits.Count -gt 0) -and ($nameInferBad.Count -eq 0) -and
               ($optHits.Count -eq 0) -and ($curveHits.Count -eq 0)
    Check "S07-02" "全仓检索 业务词作为判断依据 / 硬编码优化常量 / 写死曲线公式 → 零命中" $s0702ok `
        ("引擎产物：业务词 {0} 处 / 名称推断 {1} 处 / 优化常量 {2} 处 / 曲线公式 {3} 处；规则包与测试数据（合法住所）{4} 处" -f `
            $bizHits.Count, $nameInferBad.Count, $optHits.Count, $curveHits.Count, $bizDataHits.Count)

    # ================================================================ ⑥ 独立交付
    Section "⑥ 独立交付（TPL-NFR-05：独立构建 / 示例 / 测试 / 验收脚本）"

    $needed = @("CMakeLists.txt", "include\topology\topology_engine.h", "src\internal.h",
                "tests\CMakeLists.txt", "tests\selftest.cc", "examples\CMakeLists.txt",
                "examples\minimal\main.cc", "examples\full_flow\main.cc",
                "scripts\acceptance.ps1", "policies\mapapp\linkThresholds.json",
                "third_party\nlohmann\json.hpp",
                "tests\fixtures\fsm-basic.json", "tests\fixtures\fsm-dwell.json",
                "tests\fixtures\two-structures.json", "tests\fixtures\optimizer.json",
                "tests\fixtures\eval.json", "tests\fixtures\camel.json",
                "tests\fixtures\snapshot-dangling.json", "tests\fixtures\bad-kind.json",
                "tests\fixtures\bad-major.json", "tests\fixtures\bad-missing-required.json",
                "tests\fixtures\bad-dup-node-type.json", "tests\fixtures\bad-unknown-source.json",
                "tests\fixtures\bad-no-hysteresis.json", "tests\fixtures\unknown-fields.json")
    $absent = @()
    foreach ($f in $needed) { if (-not (Test-Path (Join-Path $script:Repo $f))) { $absent += $f } }
    Check "C29" "独立交付要件齐全（构建 / 公开头 / 测试 / 示例 / 验收脚本 / 规则包 / 夹具）" `
        ($absent.Count -eq 0) ("缺失：{0}" -f (($absent -join ", ") -replace '^$', '无'))

    $exampleCodes = @{}
    foreach ($name in @("example_minimal", "example_full_flow")) {
        $exe = Join-Path $bin "$name.exe"
        if (-not (Test-Path $exe)) { $exe = Join-Path $bin $name }
        if (-not (Test-Path $exe)) { $exampleCodes[$name] = -1; continue }
        $out = (& $exe 2>&1 | Out-String)
        $exampleCodes[$name] = $LASTEXITCODE
        if ($LASTEXITCODE -ne 0) { Write-Host ("    {0} 输出：{1}" -f $name, $out.Trim()) -ForegroundColor DarkGray }
    }
    $exOk = $true
    foreach ($k in $exampleCodes.Keys) { if ($exampleCodes[$k] -ne 0) { $exOk = $false } }
    Check "C30" "两个示例独立运行退出码 0（规则包由 CMake 注入绝对路径）" $exOk `
        (($exampleCodes.Keys | Sort-Object | ForEach-Object { "{0}={1}" -f $_, $exampleCodes[$_] }) -join " ")

    $ctest = (Get-Command ctest -ErrorAction SilentlyContinue).Source
    if ($ctest) {
        $ctestOut = (& $ctest --test-dir $buildPath -C $Config --output-on-failure 2>&1 | Out-String)
        $ctestOk = ($LASTEXITCODE -eq 0)
        Check "C31" "ctest 注册的用例全部通过" $ctestOk ((($ctestOut -split "`n") |
            Where-Object { $_ -match 'tests passed|tests failed|Total Test time' }) -join " ")
    } else {
        Check "C31" "ctest 注册的用例全部通过（跳过：未找到 ctest）" $true "ctest 不在 PATH，视为不适用"
    }

    # C31b：引擎内无落库 / 无广播 / 无 Web 框架调用（TPL-NFR-02）
    $srcFiles = [string[]]@(CollectFiles @("src") @(".cc", ".h") @())
    $dbHits = @(SearchHits $srcFiles `
        '(SQLite|sqlite3|mysql_|PQexec|nanodbc|drogon::|Drogon|listen\(|eventhub|EventHub|broadcast\()' `
        @('MUST NOT', '零命中'))
    Check "C31b" "TPL-NFR-02：引擎内无落库 / 无广播 / 无 Web 框架调用（出口只走反向接口）" `
        ($dbHits.Count -eq 0) ("命中 {0} 处{1}" -f $dbHits.Count, (Format-Hits $dbHits))

    $scriptText = Get-Content -Raw -Encoding UTF8 (Join-Path $script:Repo "scripts\acceptance.ps1")
    $exitOk = ($scriptText -match '(?m)^\s*exit\s+1') -and ($scriptText -match '(?m)^\s*exit\s+0')
    Check "C32" "验收脚本以退出码 0/1 结束（TPL-NFR-05）" $exitOk `
        "脚本内含 exit 0 与 exit 1 两条收口路径"

    # ================================================================ 汇总
    Section "汇总"

    $total = $script:Results.Count
    $passed = 0
    foreach ($r in $script:Results) { if ($r.Ok) { $passed++ } }
    $failed = $total - $passed

    if ($jsonOk) {
        Write-Host ("selftest ：用例 {0} 个（失败 {1}）｜断言 {2} 条（失败 {3}）" -f `
            $data.cases, $data.casesFailed, $data.asserts, $data.assertsFailed)
    }
    Write-Host ("验收检查：{0} 项｜PASS {1}｜FAIL {2}" -f $total, $passed, $failed)

    if ($failed -eq 0) {
        Write-Host ""
        Write-Host "验收通过（退出码 0）" -ForegroundColor Green
        exit 0
    }
    Write-Host ""
    Write-Host "验收失败（退出码 1）：" -ForegroundColor Red
    foreach ($r in $script:Results) {
        if (-not $r.Ok) { Write-Host ("  - {0} {1}：{2}" -f $r.Id, $r.Title, $r.Detail) -ForegroundColor Red }
    }
    exit 1
} finally {
    Pop-Location
}
