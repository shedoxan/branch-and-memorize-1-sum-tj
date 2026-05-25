# Exact Branch-and-Memorize Solver for 1||sum T_j

This repository contains a C++20 exact Branch-and-Memorize solver for the
single-machine total tardiness problem:

```text
1 || sum T_j,  T_j = max(C_j - d_j, 0)
```

The final experimental configuration uses the `adaptive_v3` model, the custom
memo table, exact memoization with full-key verification, terminal rules,
position filtering, Lawler basic rules, and no LB/UB pruning in the final
baseline.

## Repository Layout

- `src/` - solver core, memo tables, generator, benchmark runner, and CLI.
- `tests/` - correctness tests, including reconstruction and memo backend checks.
- `scripts/` - experiment runners and result analysis scripts.
- `results/figures/` - final figures used for the course report.
- `results/processed/` - processed CSV summaries.
- `results/latex_tables/` - compact LaTeX tables.
- `results/report.md` - generated summary of the benchmark analysis.

The external implementation from the reference article is not included. The
comparison script expects its executable path through `--article-exe`.

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Windows/MSVC the executables are also emitted to `x64/Release/`.

## Run Examples

Solve one generated instance with the final preset and reconstruct the schedule:

```powershell
.\x64\Release\bm_solver.exe --preset best-final --n 500 --seed 1 --reconstruct --reconstruction-trace
```

Run the benchmark executable directly:

```powershell
.\x64\Release\solver_bench.exe --series branching --n-list 100,200,500 --r-values 0.2 --t-values 0.6 --seeds 1,2,3
```

Regenerate processed tables and figures from raw benchmark CSV files:

```powershell
python .\analyze_bm_results.py --input-dir .\results_raw --out-dir .\results\processed --fig-dir .\results\figures --latex-dir .\results\latex_tables
```

## Results

The curated result artifacts are committed under `results/`. Figure captions and
axis labels are in Russian because they are intended for direct use in the
course report.

Key summary files:

- `results/report.md`
- `results/figures/article_wall_time_by_pair_grid.png`
- `results/figures/article_peak_memory_by_pair_grid.png`
- `results/figures/model_race_by_pair_grid.png`
- `results/figures/reconstruction_wall_overhead_old_vs_new.png`
