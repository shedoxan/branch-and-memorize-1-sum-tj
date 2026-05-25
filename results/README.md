# Benchmark Results

This directory contains the curated benchmark artifacts used in the course
report.

- `figures/` - publication-ready PNG figures with Russian captions.
- `processed/` - normalized and summarized CSV tables.
- `latex_tables/` - compact LaTeX tables generated from the processed data.
- `report.md` - generated textual summary of the experiments.

Raw benchmark logs and the external article executable are not included. To
reproduce the full pipeline, rerun the experiment scripts into `results_raw/`
and then run:

```powershell
python ..\analyze_bm_results.py --input-dir ..\results_raw --out-dir .\processed --fig-dir .\figures --latex-dir .\latex_tables
```
