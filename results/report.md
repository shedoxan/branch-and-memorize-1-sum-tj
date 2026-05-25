# Branch-and-Memorize Results Report

## Input Files

- `C:\c++\test2\results\article_code_comparison\raw_results.csv`
- `C:\c++\test2\results\article_code_comparison_r1_fixed\raw_results.csv`
- `C:\c++\test2\results\best_reconstruction_overhead\raw_results.csv`
- `C:\c++\test2\results\bounds_ablation\raw_results.csv`
- `C:\c++\test2\results\memo_backend_comparison\raw_results.csv`
- `C:\c++\test2\results\memo_backend_no_gate\raw_results.csv`
- `C:\c++\test2\results\progressive_hard_pair_race\raw_results.csv`
- `C:\c++\test2\results\progressive_model_race\raw_results.csv`
- `C:\c++\test2\results\reconstruction_overhead\raw_results.csv`

## Missing Columns

- `C:\c++\test2\results\bounds_ablation\raw_results.csv`: memo_clean_time_ms, process_memory_gate, reconstruct_order, reconstruction_trace
- `C:\c++\test2\results\memo_backend_comparison\raw_results.csv`: memo_clean_time_ms, process_memory_gate, reconstruct_order, reconstruction_trace
- `C:\c++\test2\results\progressive_hard_pair_race\raw_results.csv`: memo_clean_time_ms, process_memory_gate, reconstruct_order, reconstruction_trace
- `C:\c++\test2\results\progressive_model_race\raw_results.csv`: memo_clean_time_ms, process_memory_gate, reconstruct_order, reconstruction_trace

## Process Gate

- process-gated rows: 0
- possibly process-gated files, recorded as metadata rather than a correctness problem:
  - `C:\c++\test2\results\bounds_ablation\raw_results.csv`
  - `C:\c++\test2\results\memo_backend_comparison\raw_results.csv`
  - `C:\c++\test2\results\progressive_hard_pair_race\raw_results.csv`
  - `C:\c++\test2\results\progressive_model_race\raw_results.csv`

## Correctness

- correctness mismatches: 0

## Model

progressive_model_race alone: winner inside complete stages at largest complete n=1000 is lawler by median solved time. This is not the final model-selection claim because that race is old solve-only data with reconstruction off and process gate enabled/assumed. Final baseline model is adaptive_v3 because the later hard-pair race, which targets the difficult region used for larger runs, favors it. This choice intentionally weights hard (R,T) pairs more than easy pairs: on easy pairs model differences are usually milliseconds or tens of milliseconds, while on hard pairs the gap can be around 100,000 ms or more. Hard-pair race R=0.2,T=0.6 at n=1100 selects adaptive_v3: median 286s, versus lawler at 420s (1.47x slower). adaptive_v3 is the model carried to n=1800 in this race. The final backend/reconstruction/article comparisons therefore use adaptive_v3 with the current best reconstruct pipeline.

## Progressive Model Race

progressive_model_race measures time to prove optimum only: reconstruction is off. These are legacy solver_bench rows with process memory gate enabled/assumed; this is noted as metadata, not treated as a correctness problem. Incomplete stages are excluded from comparative plots; largest complete n=1000. The aggregate growth plot uses a log-scale y axis because solve times span several orders of magnitude; the per-(R,T) grid is generated in log scale and as a linear clipped-outlier view. At n=1000, using each pair's fastest median config, the easiest pair is (R=0.8, T=0.2) at 0.00905ms, while the hardest is (R=0.2, T=0.6) at 196s, about 2.17e7x slower. This is why aggregate-by-n plots should be read together with paired (R,T) plots.

## Hard Pair Race

progressive_hard_pair_race is also old solve-only data: reconstruction is off and process memory gate is enabled/assumed. Since this run has no OOT/OOM/ERR rows, the scheme is reported with solved time only. The primary hard pair is R=0.2,T=0.6; the hard-subset grid shows R,T = (0.2,0.4), (0.2,0.6), (0.2,0.8), and (0.4,0.6). Hard-pair race R=0.2,T=0.6 at n=1100 selects adaptive_v3: median 286s, versus lawler at 420s (1.47x slower). adaptive_v3 is the model carried to n=1800 in this race. At n=1100 the survival threshold is 1.35x best = 386s; all non-adaptive_v3 configs are above it and are marked eliminated_slow: adaptive_v1, adaptive_v2, both, lawler, szwarc.

## Bounds

bounds_ablation is plotted as paired ratios to baseline_bounds on the same instance key. The figures show, for each (R,T), median time ratio in linear scale and median node ratio; the dashed horizontal line is baseline=1.0. Objective check: all modes match baseline. The closest mode to neutral is edd_ub with median time ratio 1.03x and median node ratio 1.00x; it is still slower than baseline, so it is not selected. Simple-LB variants reduce nodes only from 1.00x to 0.99x while slowing time from 1.42x to 1.50x; their median bound-time share is 13.0%. LB memo hit rate is 0.0% where measured. Worst median time ratio is simple_lb_lb_memo_edd_ub=1.50x. Conclusion: keep LB/UB off in the final configuration. Decisions: edd_ub: nodes_down_time_up; simple_lb: nodes_down_time_up; simple_lb_edd_ub: nodes_down_time_up; simple_lb_lb_memo: nodes_down_time_up; simple_lb_lb_memo_edd_ub: nodes_down_time_up

## Backend

memo_backend_comparison uses clean no-process-gate data only for the final backend decision. Rows are paired by the same instance. Objective check: match; memo_full_key_verification=ok; exact memo=ok. Overall median custom/std time ratio is 0.93x; median memory ratio is 0.86x. Custom win-rate is 89.3% overall, 25.0% at n=50, and 100.0% for n>=100. Memory is plotted as one median line because the maximum pair-level (R,T) spread is 1.85e-06. Conclusion: keep custom memo backend; n=50 is noisy, but from n>=100 custom is consistently faster and uses less memory.

## Reconstruction

reconstruction_overhead compares legacy wall-overhead data with the current trace-based reconstruction run. The old CSV has no pure reconstruction_time_ms field, so old-vs-new speed is shown with paired process wall overhead; the primary current metric is trace_reconstruction_time_ms. New trace reconstruction stays at or below 4.59ms median actual reconstruction time, with maximum median wall overhead 2.1% versus old legacy minimum median wall overhead 15.3%. Trace fallback max is 0; objective/order checks are 100%. The cost is memory: median trace/solve-only memo ratio reaches 1.91x. Conclusion: keep the new trace reconstruction path; it makes final schedule recovery fast, with an explicit memory tradeoff.

## Article Comparison

article_code_comparison is the final external comparison: our solver is run with reconstruction on, adaptive final model, custom memo, exact memo/full-key verification, LB/UB off, and process gate off. All primary time/memory ratios are computed only on comparable rows where both programs solved; ratio medians are paired by instance and are not ratios of aggregate medians. Incomplete n stages and known invalid runner rows are excluded from primary plots. Raw rows=2891, excluded invalid runner rows=480, rows used=2411, complete-grid rows=2400, incomplete n=1200. Our solved rate is 100.0%; article solved rate is 100.0%, with 0 article ERROR rows. Objective mismatches among solved comparisons: 0. Primary median wall-time ratio ours/article is 1.01x; primary median peak working-set ratio is 0.99x. Best time-ratio regions for ours: R=0.2,T=0.6: 0.72x; R=0.2,T=0.4: 0.76x; R=0.2,T=0.8: 0.94x. Worst time-ratio regions for ours: R=0.8,T=0.6: 1.36x; R=0.6,T=0.4: 1.26x; R=0.6,T=0.6: 1.20x. Final-config validation: ok. Solved-rate plots are intentionally omitted because both programs solve all valid rows after the R=1.0 filename fix. Use article_objective_match_by_n.png and article_objective_match_by_RT.png for correctness agreement; use article_wall_time_by_pair_grid.png, article_wall_time_by_pair_grid_linear.png, and article_peak_memory_by_pair_grid.png as the main direct comparisons against Branch.exe by (R,T). Peak working set is an external resident-memory process sample, not memo-table bytes; it includes allocator/runtime overhead. Use ratio heatmaps and the full/hard-set time-memory tradeoff plots as compact summaries.

## Generated Artifacts

- `processed\normalized_runs.csv`
- `processed\general_summary.csv`
- `processed\correctness_mismatches.csv`
- `processed\process_gate_contamination.csv`
- `latex_tables\table_final_config.tex`
- `processed\model_race_incomplete_stages.csv`
- `processed\model_race_summary.csv`
- `processed\model_race_winners_by_n.csv`
- `processed\model_race_wins_by_seed.csv`
- `figures\model_race_growth.png`
- `processed\model_race_by_pair_summary.csv`
- `processed\model_race_pair_variability.csv`
- `latex_tables\table_model_race_pair_variability.tex`
- `figures\model_race_by_pair_grid.png`
- `figures\model_race_by_pair_grid_linear.png`
- `figures\model_race_relative_to_best_by_RT.png`
- `processed\model_race_relative_by_RT.csv`
- `figures\model_race_winner_heatmap.png`
- `latex_tables\table_model_race.tex`
- `processed\hard_pair_summary.csv`
- `processed\hard_subset_summary.csv`
- `figures\hard_pair_growth.png`
- `figures\hard_pair_growth_linear.png`
- `figures\hard_subset_by_pair_grid.png`
- `figures\hard_subset_by_pair_grid_linear.png`
- `processed\hard_race_elimination.csv`
- `latex_tables\table_hard_pair.tex`
- `latex_tables\table_hard_race_elimination.tex`
- `processed\bounds_ablation_summary.csv`
- `processed\bounds_ablation_paired_ratios.csv`
- `figures\bounds_time_ratio_by_pair_linear.png`
- `figures\bounds_node_ratio_by_pair.png`
- `latex_tables\table_bounds_ablation.tex`
- `processed\backend_comparison_clean.csv`
- `processed\backend_comparison_preliminary_process_gate.csv`
- `processed\backend_process_gate_effect.csv`
- `processed\backend_comparison_by_n.csv`
- `processed\backend_memory_ratio_spread_by_n.csv`
- `figures\backend_custom_win_rate.png`
- `processed\backend_custom_win_rate.csv`
- `figures\backend_time_ratio_distribution_by_n.png`
- `figures\backend_time_ratio_by_pair_grid.png`
- `figures\backend_memory_ratio_by_n.png`
- `figures\backend_time_memory_tradeoff.png`
- `latex_tables\table_backend_clean.tex`
- `processed\reconstruction_summary.csv`
- `processed\reconstruction_before_after.csv`
- `figures\reconstruction_wall_overhead_old_vs_new.png`
- `figures\reconstruction_actual_trace_time_by_n.png`
- `figures\reconstruction_trace_memory_ratio_by_n.png`
- `latex_tables\table_reconstruction.tex`
- `processed\article_comparison_summary.csv`
- `processed\article_comparison_by_n.csv`
- `processed\article_comparison_by_RT.csv`
- `processed\article_comparison_by_n_RT.csv`
- `processed\article_comparable_instances.csv`
- `processed\article_not_comparable.csv`
- `processed\article_invalid_runner_rows.csv`
- `processed\article_config_validation.csv`
- `figures\article_objective_match_by_n.png`
- `figures\article_objective_match_by_RT.png`
- `figures\article_wall_time_by_pair_grid.png`
- `figures\article_wall_time_by_pair_grid_linear.png`
- `figures\article_peak_memory_by_pair_grid.png`
- `figures\article_time_ratio_distribution_by_n.png`
- `figures\article_time_ratio_heatmap_RT.png`
- `figures\article_memory_ratio_distribution_by_n.png`
- `figures\article_memory_ratio_heatmap_RT.png`
- `figures\article_time_memory_tradeoff.png`
- `figures\article_time_memory_tradeoff_hard_set.png`
- `latex_tables\table_article_comparison.tex`
- `latex_tables\table_article_RT.tex`

## Notes

- progressive_model_race incomplete stages excluded from comparative plots/ranking: n=1100 (1/6 configs)
