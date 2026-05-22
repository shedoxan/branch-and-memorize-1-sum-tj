param(
    [string]$Pairs = "0.2:0.6;0.2:0.4;0.4:0.6",
    [string]$NValues = "100,300,500,700,900,1100",
    [string]$Seeds = "0,1,2,3,4",
    [switch]$Fresh,
    [switch]$SkipBuild,
    [switch]$SkipTests
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Runner = Join-Path $ScriptDir "run_reconstruction_overhead.py"
$argsList = @(
    $Runner,
    "--pairs", $Pairs,
    "--n-values", $NValues,
    "--seeds", $Seeds
)
if ($Fresh) { $argsList += "--fresh" }
if ($SkipBuild) { $argsList += "--skip-build" }
if ($SkipTests) { $argsList += "--skip-tests" }
python $argsList
