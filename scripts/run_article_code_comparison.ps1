param(
    [string]$RValues = "0.2,0.4,0.6,0.8,1.0",
    [string]$TValues = "0.2,0.4,0.6,0.8",
    [string]$NValues = "50,100,200,300,400,500,600,700,800,900,1000,1100,1200,1300",
    [string]$Seeds = "0,1,2,3,4,5,6,7,8,9",
    [string]$SharedWorkDir = "",
    [int]$FileSeedOffset = 1,
    [string]$ArticleExe = "",
    [string]$ArticleConfig = "",
    [string]$ArticleCommandTemplate = "",
    [double]$MemoryLimitGb = 12.0,
    [int]$TimeoutUntil1000Sec = 1200,
    [int]$TimeoutAfter1000Sec = 12600,
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
    "--seeds", $Seeds,
    "--file-seed-offset", "$FileSeedOffset",
    "--memory-limit-gb", "$MemoryLimitGb",
    "--timeout-until-1000-sec", "$TimeoutUntil1000Sec",
    "--timeout-after-1000-sec", "$TimeoutAfter1000Sec"
)
if ($SharedWorkDir -ne "") { $argsList += @("--shared-work-dir", $SharedWorkDir) }
if ($ArticleExe -ne "") { $argsList += @("--article-exe", $ArticleExe) }
if ($ArticleConfig -ne "") { $argsList += @("--article-config", $ArticleConfig) }
if ($ArticleCommandTemplate -ne "") { $argsList += @("--article-command-template", $ArticleCommandTemplate) }
if ($Fresh) { $argsList += "--fresh" }
if ($SkipBuild) { $argsList += "--skip-build" }
if ($SkipTests) { $argsList += "--skip-tests" }
python $argsList
