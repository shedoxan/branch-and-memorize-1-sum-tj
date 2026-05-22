param(
    [string]$RValues = "0.2,0.4,0.6,0.8,1.0",
    [string]$TValues = "0.2,0.4,0.6,0.8",
    [string]$NValues = "50,100,200,300,400,500,600,700,800,900,1000,1100,1200,1300",
    [string]$Seeds = "0,1,2,3,4,5,6,7,8,9",
    [string]$DataRoot = "",
    [string]$ArticleExe = "",
    [string]$ArticleCommandTemplate = "",
    [switch]$Fresh,
    [switch]$SkipBuild,
    [switch]$SkipTests
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Runner = Join-Path $ScriptDir "run_article_code_comparison.py"
$argsList = @(
    $Runner,
    "--R-values", $RValues,
    "--T-values", $TValues,
    "--n-values", $NValues,
    "--seeds", $Seeds
)
if ($DataRoot -ne "") { $argsList += @("--data-root", $DataRoot) }
if ($ArticleExe -ne "") { $argsList += @("--article-exe", $ArticleExe) }
if ($ArticleCommandTemplate -ne "") { $argsList += @("--article-command-template", $ArticleCommandTemplate) }
if ($Fresh) { $argsList += "--fresh" }
if ($SkipBuild) { $argsList += "--skip-build" }
if ($SkipTests) { $argsList += "--skip-tests" }
python $argsList
