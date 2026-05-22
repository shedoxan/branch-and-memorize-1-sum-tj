param(
    [string]$Pairs = "0.2:0.6;0.2:0.4;0.2:0.8;0.4:0.6",
    [string]$NValues = "50,100,200,300,500,700,900,1100,1300",
    [string]$Seeds = "0,1,2,3,4,5,6,7,8,9",
    [string]$Backends = "custom,std_unordered",
    [switch]$Fresh,
    [switch]$SkipBuild,
    [switch]$SkipTests
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Runner = Join-Path $ScriptDir "run_memo_backend_comparison.py"
$argsList = @(
    $Runner,
    "--pairs", $Pairs,
    "--n-values", $NValues,
    "--seeds", $Seeds,
    "--backends", $Backends
)
if ($Fresh) { $argsList += "--fresh" }
if ($SkipBuild) { $argsList += "--skip-build" }
if ($SkipTests) { $argsList += "--skip-tests" }
python $argsList
