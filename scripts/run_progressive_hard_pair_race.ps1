param(
    [string]$Models = "lawler,szwarc,both,adaptive_v1,adaptive_v2,adaptive_v3",
    [string]$NValues = "1100,1200,1300,1400,1500,1600,1700,1800,1900,2000",
    [string]$RValues = "0.2,0.4,0.6,0.8,1.0",
    [string]$TValues = "0.2,0.4,0.6,0.8",
    [string]$Seeds = "0,1,2,3,4,5,6,7,8,9",
    [string]$PrimaryHardPair = "0.2:0.6",
    [string]$HardSubset = "0.2:0.6;0.2:0.4;0.2:0.8;0.4:0.6",
    [double]$MemoryLimitGb = 12,
    [switch]$Fresh,
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$DryRun
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Runner = Join-Path $ScriptDir "progressive_hard_pair_race.py"

$ArgsList = @(
    $Runner,
    "--models", $Models,
    "--n-values", $NValues,
    "--R-values", $RValues,
    "--T-values", $TValues,
    "--seeds", $Seeds,
    "--primary-hard-pair", $PrimaryHardPair,
    "--hard-subset", $HardSubset,
    "--memory-limit-gb", "$MemoryLimitGb"
)

if ($Fresh) { $ArgsList += "--fresh" }
if ($SkipBuild) { $ArgsList += "--skip-build" }
if ($SkipTests) { $ArgsList += "--skip-tests" }
if ($DryRun) { $ArgsList += "--dry-run" }

python @ArgsList
