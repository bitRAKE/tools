param(
    [string]$RawwhpExe = "_test\rawwhp.exe",
    [string]$ExpectedPath = "tests\rawwhp\expected\strict_exits.json",
    [string]$ArtifactRoot = "tests\rawwhp\artifacts",
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}

function Parse-HexU64 {
    param([Parameter(Mandatory = $true)][string]$Text)
    $t = $Text.Trim()
    if ($t.StartsWith('0x', [System.StringComparison]::OrdinalIgnoreCase)) {
        $t = $t.Substring(2)
    }
    return [Convert]::ToUInt64($t, 16)
}

function Ensure-Probes {
    param(
        [Parameter(Mandatory = $true)][string]$Dir
    )

    New-Item -ItemType Directory -Force $Dir | Out-Null
    $probes = [ordered]@{
        hlt    = [byte[]](0xF4)
        int3   = [byte[]](0xCC, 0xF4)
        ud2    = [byte[]](0x0F, 0x0B, 0xF4)
        io_in  = [byte[]](0xE4, 0x80, 0xF4)
        io_out = [byte[]](0xE6, 0x80, 0xF4)
        cpuid  = [byte[]](0x0F, 0xA2, 0xF4)
        rdmsr  = [byte[]](0x31, 0xC9, 0x0F, 0x32, 0xF4)
        wrmsr  = [byte[]](0x31, 0xC9, 0x31, 0xC0, 0x31, 0xD2, 0x0F, 0x30, 0xF4)
        rdmsr_unk = [byte[]](0xB9, 0xEF, 0xBE, 0xAD, 0xDE, 0x0F, 0x32, 0xF4)
        wrmsr_unk = [byte[]](0xB9, 0xEF, 0xBE, 0xAD, 0xDE, 0x31, 0xC0, 0x31, 0xD2, 0x0F, 0x30, 0xF4)
        div0   = [byte[]](0x31, 0xC0, 0xF7, 0xF0, 0xF4)
        cli    = [byte[]](0xFA, 0xF4)
        vmcall = [byte[]](0x0F, 0x01, 0xC1, 0xF4)
    }

    $result = @{}
    foreach ($name in $probes.Keys) {
        $path = Join-Path $Dir "$name.bin"
        [System.IO.File]::WriteAllBytes($path, $probes[$name])
        $result[$name] = $path
    }
    return $result
}

function Ensure-Rawwhp {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath
    )

    if (Test-Path -LiteralPath $ExePath) {
        return
    }

    $repo = Get-RepoRoot
    Write-Host "Building rawwhp -> $ExePath"
    & cl /nologo /W4 /O2 /DUNICODE /D_UNICODE "/Fe:$ExePath" rawwhp.c WinHvPlatform.lib
    if ($LASTEXITCODE -ne 0) {
        throw "cl build failed (exit $LASTEXITCODE)"
    }
}

function Invoke-RawwhpCase {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][string[]]$Args,
        [Parameter(Mandatory = $true)][string]$ReportPath
    )

    if (Test-Path -LiteralPath $ReportPath) {
        Remove-Item -LiteralPath $ReportPath -Force
    }

    & $Exe @Args | Out-Host
    $code = $LASTEXITCODE
    $report = $null
    if (Test-Path -LiteralPath $ReportPath) {
        $report = Get-Content -Raw -LiteralPath $ReportPath | ConvertFrom-Json
    }
    return [pscustomobject]@{
        exit_code = $code
        report = $report
    }
}

function Assert-True {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

function Get-ExpectedExitCode {
    param([Parameter(Mandatory = $true)][string]$ExitReason)
    switch ($ExitReason) {
        'X64Halt' { return 0 }
        'Hypercall' { return 0 }
        'Timeout' { return 3 }
        default { return 4 }
    }
}

function Assert-SubsetObject {
    param(
        [Parameter(Mandatory = $true)]$Expected,
        [Parameter(Mandatory = $true)]$Actual,
        [Parameter(Mandatory = $true)][string]$Path
    )

    foreach ($prop in $Expected.PSObject.Properties) {
        $name = $prop.Name
        $exp = $prop.Value
        $actProp = $Actual.PSObject.Properties[$name]
        if ($null -eq $actProp) {
            throw "$Path.$name was not present in report"
        }
        $act = $actProp.Value

        if ($exp -is [System.Management.Automation.PSCustomObject]) {
            Assert-SubsetObject -Expected $exp -Actual $act -Path "$Path.$name"
            continue
        }

        if ($exp -is [bool]) {
            if ([bool]$act -ne $exp) {
                throw "$Path.$name expected '$exp' got '$act'"
            }
            continue
        }

        if ("$act" -ne "$exp") {
            throw "$Path.$name expected '$exp' got '$act'"
        }
    }
}

$repoRoot = Get-RepoRoot
Push-Location $repoRoot
try {
    if (-not $SkipBuild) {
        Ensure-Rawwhp -ExePath $RawwhpExe
    }
    if (-not (Test-Path -LiteralPath $RawwhpExe)) {
        throw "rawwhp executable not found: $RawwhpExe"
    }

    New-Item -ItemType Directory -Force $ArtifactRoot | Out-Null
    $timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $runDir = Join-Path $ArtifactRoot "run_$timestamp"
    New-Item -ItemType Directory -Force $runDir | Out-Null

    $probeDir = Join-Path $PSScriptRoot 'probes\generated'
    $probeFiles = Ensure-Probes -Dir $probeDir
    $expected = Get-Content -Raw -LiteralPath $ExpectedPath | ConvertFrom-Json

    Write-Host "[1/4] Mapping granularity checks"
    $m1Report = Join-Path $runDir 'map_same_page.json'
    $m1 = Invoke-RawwhpCase -Exe $RawwhpExe -ReportPath $m1Report -Args @(
        '/area', '10000', '10', $probeFiles.hlt,
        '/area', '10020', '10', $probeFiles.hlt,
        '/at', '10000',
        '/ticks', '40',
        '/report', $m1Report
    )
    Assert-True -Condition ($m1.exit_code -eq 0) -Message "same-page mapping case failed (exit $($m1.exit_code))"
    Assert-True -Condition ($m1.report.maps.Count -eq 1) -Message "same-page mapping expected maps=1 got $($m1.report.maps.Count)"

    $m2Report = Join-Path $runDir 'map_far_pages.json'
    $m2 = Invoke-RawwhpCase -Exe $RawwhpExe -ReportPath $m2Report -Args @(
        '/area', '10000', '10', $probeFiles.hlt,
        '/area', '12000', '10', $probeFiles.hlt,
        '/at', '10000',
        '/ticks', '40',
        '/report', $m2Report
    )
    Assert-True -Condition ($m2.exit_code -eq 0) -Message "far-pages mapping case failed (exit $($m2.exit_code))"
    Assert-True -Condition ($m2.report.maps.Count -eq 2) -Message "far-pages mapping expected maps=2 got $($m2.report.maps.Count)"

    $ovReport = Join-Path $runDir 'overlap_reject.json'
    $ov = Invoke-RawwhpCase -Exe $RawwhpExe -ReportPath $ovReport -Args @(
        '/area', '10000', '100', $probeFiles.hlt,
        '/area', '10080', '40', $probeFiles.hlt,
        '/at', '10000',
        '/report', $ovReport
    )
    Assert-True -Condition ($ov.exit_code -eq 2) -Message "overlap rejection expected exit=2 got $($ov.exit_code)"

    Write-Host "[2/4] Dump behavior checks"
    $dumpStdoutReport = Join-Path $runDir 'dump_stdout.json'
    $ds = Invoke-RawwhpCase -Exe $RawwhpExe -ReportPath $dumpStdoutReport -Args @(
        '/area', '10000', '20', $probeFiles.hlt,
        '/at', '10000',
        '/ticks', '40',
        '/dump', '10000', '10',
        '/report', $dumpStdoutReport
    )
    Assert-True -Condition ($ds.exit_code -eq 0) -Message "stdout dump case failed (exit $($ds.exit_code))"
    Assert-True -Condition ($ds.report.dumps[0].target -eq 'stdout') -Message "stdout dump target mismatch"
    Assert-True -Condition ($ds.report.dumps[0].status -eq 'ok') -Message "stdout dump status mismatch"

    $dumpFile = Join-Path $runDir 'dump_file.bin'
    $dumpFileReport = Join-Path $runDir 'dump_file.json'
    $df = Invoke-RawwhpCase -Exe $RawwhpExe -ReportPath $dumpFileReport -Args @(
        '/area', '10000', '20', $probeFiles.hlt,
        '/at', '10000',
        '/ticks', '40',
        '/dump', '10000', '10', $dumpFile,
        '/report', $dumpFileReport
    )
    Assert-True -Condition ($df.exit_code -eq 0) -Message "file dump case failed (exit $($df.exit_code))"
    Assert-True -Condition (Test-Path -LiteralPath $dumpFile) -Message "dump file was not created"
    $size = (Get-Item -LiteralPath $dumpFile).Length
    Assert-True -Condition ($size -eq 0x10) -Message ("dump file size expected 0x10 got 0x{0:X}" -f $size)
    Assert-True -Condition ($df.report.dumps[0].status -eq 'ok') -Message "file dump status mismatch"

    $badDumpReport = Join-Path $runDir 'dump_invalid.json'
    $bd = Invoke-RawwhpCase -Exe $RawwhpExe -ReportPath $badDumpReport -Args @(
        '/area', '10000', '10', $probeFiles.hlt,
        '/at', '10000',
        '/ticks', '40',
        '/dump', '18000', '10',
        '/report', $badDumpReport
    )
    Assert-True -Condition ($bd.exit_code -eq 2) -Message "invalid dump range expected exit=2 got $($bd.exit_code)"

    Write-Host "[3/4] Capability gate"
    $capReport = Join-Path $runDir 'capability_probe.json'
    $cap = Invoke-RawwhpCase -Exe $RawwhpExe -ReportPath $capReport -Args @(
        '/area', '10000', '20', $probeFiles.hlt,
        '/at', '10000',
        '/ticks', '40',
        '/report', $capReport
    )
    Assert-True -Condition ($cap.exit_code -eq 0) -Message "capability probe failed (exit $($cap.exit_code))"
    $caps = $cap.report.capabilities
    $req = $expected.required_capabilities
    $requiredExtSupported = if ($req.PSObject.Properties['extended_vm_exits_supported']) {
        [bool]$req.extended_vm_exits_supported
    } else {
        [bool]$req.extended_vm_exits_enabled
    }
    $requiredExcSupported = if ($req.PSObject.Properties['exception_bitmap_supported']) {
        [bool]$req.exception_bitmap_supported
    } else {
        [bool]$req.exception_bitmap_enabled
    }
    $requiredMsrSupported = if ($req.PSObject.Properties['msr_exit_bitmap_supported']) {
        [bool]$req.msr_exit_bitmap_supported
    } else {
        $false
    }

    $hostSupports = (-not $requiredExtSupported -or [bool]$caps.extended_vm_exits_supported) -and
        (-not $requiredExcSupported -or [bool]$caps.exception_bitmap_supported) -and
        (-not $requiredMsrSupported -or [bool]$caps.msr_exit_bitmap_supported)
    if (-not $hostSupports) {
        Write-Host ("[SKIP] strict exit matrix not run. required caps missing: " +
            "extended_vm_exits_supported={0}, exception_bitmap_supported={1}, msr_exit_bitmap_supported={2}" -f
            $caps.extended_vm_exits_supported, $caps.exception_bitmap_supported, $caps.msr_exit_bitmap_supported)
        Write-Host ("Artifacts: {0}" -f $runDir)
        exit 0
    }

    if (($requiredExtSupported -and -not [bool]$caps.extended_vm_exits_enabled) -or
        ($requiredExcSupported -and -not [bool]$caps.exception_bitmap_enabled) -or
        ($requiredMsrSupported -and -not [bool]$caps.msr_exit_bitmap_enabled)) {
        throw ("host capability is present but partition exits were not enabled " +
            "(extended_vm_exits_enabled={0}, exception_bitmap_enabled={1}, msr_exit_bitmap_enabled={2})" -f
            $caps.extended_vm_exits_enabled, $caps.exception_bitmap_enabled, $caps.msr_exit_bitmap_enabled)
    }

    Write-Host "[4/4] Strict exit matrix"
    $pass = 0
    $total = $expected.cases.Count
    foreach ($case in $expected.cases) {
        $areaStart = if ($case.PSObject.Properties['area_start']) { $case.area_start } else { $expected.defaults.area_start }
        $areaLength = if ($case.PSObject.Properties['area_length']) { $case.area_length } else { $expected.defaults.area_length }
        $at = if ($case.PSObject.Properties['at']) { $case.at } else { $expected.defaults.at }
        $ticks = if ($case.PSObject.Properties['ticks']) { $case.ticks } else { $expected.defaults.ticks }
        $mode = "$($case.mode)"
        $modeSupportsDefaultCpl = ($mode -eq 'protected' -or $mode -eq 'long')
        $cpl = if ($case.PSObject.Properties['cpl']) {
            "$($case.cpl)"
        } elseif ($modeSupportsDefaultCpl -and $expected.defaults.PSObject.Properties['cpl']) {
            "$($expected.defaults.cpl)"
        } else {
            $null
        }
        $probePath = $probeFiles[$case.probe]
        if (-not $probePath) {
            throw "missing probe '$($case.probe)' for case '$($case.id)'"
        }

        $reportPath = Join-Path $runDir ("strict_{0}.json" -f $case.id)
        $args = [System.Collections.Generic.List[string]]::new()
        $args.Add('/area')
        $args.Add(("{0:X}" -f (Parse-HexU64 $areaStart)))
        $args.Add(("{0:X}" -f (Parse-HexU64 $areaLength)))
        $args.Add($probePath)
        $args.Add('/mode')
        $args.Add($mode)
        if ($null -ne $cpl -and $cpl.Length -gt 0) {
            $args.Add('/cpl')
            $args.Add($cpl)
        }
        $args.Add('/at')
        $args.Add(("{0:X}" -f (Parse-HexU64 $at)))
        $args.Add('/ticks')
        $args.Add(("{0:X}" -f (Parse-HexU64 $ticks)))
        $args.Add('/report')
        $args.Add($reportPath)

        $res = Invoke-RawwhpCase -Exe $RawwhpExe -ReportPath $reportPath -Args $args.ToArray()

        $expectedExitReason = [string]$case.expect.exit_reason
        $expectedExitCode = Get-ExpectedExitCode -ExitReason $expectedExitReason
        Assert-True -Condition ($res.exit_code -eq $expectedExitCode) `
            -Message ("case {0} expected process exit {1} got {2}" -f $case.id, $expectedExitCode, $res.exit_code)

        Assert-True -Condition ($null -ne $res.report) -Message "case $($case.id) missing report"
        Assert-True -Condition ($res.report.run.exit_reason -eq $expectedExitReason) `
            -Message ("case {0} expected exit_reason {1} got {2}" -f $case.id, $expectedExitReason, $res.report.run.exit_reason)

        $expectedResult = if ($expectedExitReason -eq 'X64Halt' -or $expectedExitReason -eq 'Hypercall') { 'success' } else { 'vm_exit' }
        Assert-True -Condition ($res.report.run.result -eq $expectedResult) `
            -Message ("case {0} expected run.result {1} got {2}" -f $case.id, $expectedResult, $res.report.run.result)

        if ($case.expect.PSObject.Properties['details']) {
            Assert-SubsetObject -Expected $case.expect.details -Actual $res.report.run.details -Path "case:$($case.id).details"
        }

        $pass++
    }

    Write-Host ("PASS strict matrix: {0}/{1}" -f $pass, $total)
    Write-Host ("Artifacts: {0}" -f $runDir)
}
finally {
    Pop-Location
}
