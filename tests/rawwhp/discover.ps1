param(
    [string]$RawwhpExe = "_test\rawwhp.exe",
    [string]$ArtifactRoot = "tests\rawwhp\artifacts",
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
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

function Add-JsonLine {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Object
    )
    ($Object | ConvertTo-Json -Depth 10 -Compress) | Add-Content -LiteralPath $Path
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
    $runDir = Join-Path $ArtifactRoot "discovery_$timestamp"
    New-Item -ItemType Directory -Force $runDir | Out-Null

    $probeDir = Join-Path $PSScriptRoot 'probes\generated'
    $probeFiles = Ensure-Probes -Dir $probeDir

    $jsonl = Join-Path $runDir 'matrix.jsonl'
    New-Item -ItemType File -Force $jsonl | Out-Null

    $modes = @('real', 'protected', 'long', 'unreal')
    $probeOrder = @('hlt', 'int3', 'ud2', 'io_in', 'io_out', 'cpuid', 'rdmsr', 'wrmsr', 'rdmsr_unk', 'wrmsr_unk', 'div0', 'cli', 'vmcall')

    $capsSaved = $false
    foreach ($mode in $modes) {
        $cplList = if ($mode -eq 'protected' -or $mode -eq 'long') { @('3', '0') } else { @($null) }
        foreach ($cpl in $cplList) {
            foreach ($probe in $probeOrder) {
                $suffix = if ($null -ne $cpl) { "{0}_cpl{1}_{2}.json" -f $mode, $cpl, $probe } else { "{0}_{1}.json" -f $mode, $probe }
                $reportPath = Join-Path $runDir $suffix

                $args = [System.Collections.Generic.List[string]]::new()
                $args.Add('/area')
                $args.Add('10000')
                $args.Add('100')
                $args.Add($probeFiles[$probe])
                $args.Add('/mode')
                $args.Add($mode)
                if ($null -ne $cpl) {
                    $args.Add('/cpl')
                    $args.Add($cpl)
                }
                $args.Add('/at')
                $args.Add('10000')
                $args.Add('/ticks')
                $args.Add('2000')
                $args.Add('/report')
                $args.Add($reportPath)

                $res = Invoke-RawwhpCase -Exe $RawwhpExe -ReportPath $reportPath -Args $args.ToArray()

                $caps = $null
                if ($null -ne $res.report) {
                    $caps = $res.report.capabilities
                    if (-not $capsSaved) {
                        ($caps | ConvertTo-Json -Depth 5) | Set-Content -LiteralPath (Join-Path $runDir 'host_caps.json')
                        $capsSaved = $true
                    }
                }

                Add-JsonLine -Path $jsonl -Object ([pscustomobject]@{
                        kind = 'probe'
                        mode = $mode
                        cpl = $cpl
                        probe = $probe
                        exit_code = $res.exit_code
                        run_result = if ($null -ne $res.report) { $res.report.run.result } else { $null }
                        exit_reason = if ($null -ne $res.report) { $res.report.run.exit_reason } else { $null }
                        rip = if ($null -ne $res.report) { $res.report.run.rip } else { $null }
                        extended_vm_exits_supported = if ($null -ne $caps) { [bool]$caps.extended_vm_exits_supported } else { $null }
                        extended_vm_exits_supported_mask = if ($null -ne $caps) { "$($caps.extended_vm_exits_supported_mask)" } else { $null }
                        extended_vm_exits_enabled = if ($null -ne $caps) { [bool]$caps.extended_vm_exits_enabled } else { $null }
                        exception_bitmap_supported = if ($null -ne $caps) { [bool]$caps.exception_bitmap_supported } else { $null }
                        exception_bitmap_supported_mask = if ($null -ne $caps) { "$($caps.exception_bitmap_supported_mask)" } else { $null }
                        exception_bitmap_enabled = if ($null -ne $caps) { [bool]$caps.exception_bitmap_enabled } else { $null }
                        msr_exit_bitmap_supported = if ($null -ne $caps) { [bool]$caps.msr_exit_bitmap_supported } else { $null }
                        msr_exit_bitmap_supported_mask = if ($null -ne $caps) { "$($caps.msr_exit_bitmap_supported_mask)" } else { $null }
                        msr_exit_bitmap_enabled = if ($null -ne $caps) { [bool]$caps.msr_exit_bitmap_enabled } else { $null }
                        report = (Resolve-Path -LiteralPath $reportPath).Path
                    })
            }
        }
    }

    $mapCases = @(
        @{
            id = 'same_page'
            args = @('/area', '10000', '10', $probeFiles.hlt, '/area', '10020', '10', $probeFiles.hlt, '/at', '10000', '/ticks', '40')
        },
        @{
            id = 'far_pages'
            args = @('/area', '10000', '10', $probeFiles.hlt, '/area', '12000', '10', $probeFiles.hlt, '/at', '10000', '/ticks', '40')
        },
        @{
            id = 'cross_page'
            args = @('/area', '10FF0', '40', $probeFiles.hlt, '/area', '11040', '10', $probeFiles.hlt, '/at', '10FF0', '/ticks', '40')
        }
    )

    foreach ($case in $mapCases) {
        $reportPath = Join-Path $runDir ("map_{0}.json" -f $case.id)
        $args = @($case.args + @('/report', $reportPath))
        $res = Invoke-RawwhpCase -Exe $RawwhpExe -ReportPath $reportPath -Args $args
        Add-JsonLine -Path $jsonl -Object ([pscustomobject]@{
                kind = 'map_probe'
                id = $case.id
                exit_code = $res.exit_code
                map_count = if ($null -ne $res.report) { $res.report.maps.Count } else { $null }
                report = (Resolve-Path -LiteralPath $reportPath).Path
            })
    }

    Write-Host "Discovery artifacts:"
    Write-Host ("  {0}" -f $runDir)
    Write-Host ("  {0}" -f $jsonl)
    if (Test-Path -LiteralPath (Join-Path $runDir 'host_caps.json')) {
        Write-Host ("  {0}" -f (Join-Path $runDir 'host_caps.json'))
    }
}
finally {
    Pop-Location
}
