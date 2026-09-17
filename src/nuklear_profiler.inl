static void render_gpu_profiler_ui(Renderer *r) {
    if (!r->vk.enable_graphics_profiler)
        return;
    struct nk_context *ctx = &r->ui.context;
    if (!g_gpu_profiler_ui.open) {
        if (nk_begin(ctx, "Profiler hidden", nk_rect(280, 10, 180, 85), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
            nk_layout_row_dynamic(ctx, 26, 2);
            nk_label(ctx, "Profiler", NK_TEXT_LEFT);
            if (nk_button_label(ctx, "Show"))
                g_gpu_profiler_ui.open = true;
        }
        nk_end(ctx);
        return;
    }
    if (!nk_begin(ctx, "GPU Profiler", nk_rect(280, 10, 820, 650),
                  NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE | NK_WINDOW_MINIMIZABLE)) {
        nk_end(ctx);
        return;
    }
    nk_layout_row_dynamic(ctx, 26, 4);
    g_gpu_profiler_ui.paused = nk_check_label(ctx, "Pause", g_gpu_profiler_ui.paused);
    g_gpu_profiler_ui.show_pipeline_stats = nk_check_label(ctx, "Pipeline Stats", g_gpu_profiler_ui.show_pipeline_stats);
    if (nk_button_label(ctx, "Reset Min/Max")) {
        forEach(i, MAX_RECORDED_PASSES) {
            g_gpu_profiler_ui.pass_stats[i].min_ms = g_gpu_profiler_ui.pass_stats[i].time_ms;
            g_gpu_profiler_ui.pass_stats[i].max_ms = g_gpu_profiler_ui.pass_stats[i].time_ms;
        }
    }
    if (nk_button_label(ctx, "Close"))
        g_gpu_profiler_ui.open = false;

    double frame_ms = ns_to_ms(r->cpu_frame_ns);
    double active_ms = ns_to_ms(r->cpu_active_ns);
    double gpu_ms = g_gpu_profiler_ui.total_gpu_time_ms;
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_label_colored(ctx, "Frame Metrics", NK_TEXT_LEFT, nk_rgb(75, 205, 255));
    nk_layout_row_dynamic(ctx, 22, 3);
    nk_label(ctx, "Metric", NK_TEXT_LEFT);
    nk_label(ctx, "Current", NK_TEXT_LEFT);
    nk_label(ctx, "Details", NK_TEXT_LEFT);
    nk_label(ctx, "Frame time", NK_TEXT_LEFT);
    nk_labelf(ctx, NK_TEXT_LEFT, "%.3f ms", frame_ms);
    nk_labelf(ctx, NK_TEXT_LEFT, "%.1f FPS", frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0);
    nk_label(ctx, "CPU active time", NK_TEXT_LEFT);
    nk_labelf(ctx, NK_TEXT_LEFT, "%.3f ms", active_ms);
    nk_labelf(ctx, NK_TEXT_LEFT, "%.1f%% of frame", frame_ms > 0.0 ? active_ms / frame_ms * 100.0 : 0.0);
    nk_label(ctx, "CPU waiting time", NK_TEXT_LEFT);
    nk_labelf(ctx, NK_TEXT_LEFT, "%.3f ms", ns_to_ms(r->cpu_wait_ns));
    nk_labelf(ctx, NK_TEXT_LEFT, "EMA %.3f ms", ns_to_ms(r->cpu_wait_accum_ns));
    nk_label(ctx, "GPU frame time", NK_TEXT_LEFT);
    nk_labelf(ctx, NK_TEXT_LEFT, "%.3f ms", gpu_ms);
    nk_labelf(ctx, NK_TEXT_LEFT, "%.1f%% of CPU frame", frame_ms > 0.0 ? MIN(gpu_ms / frame_ms * 100.0, 100.0) : 0.0);
    nk_label(ctx, "Nuklear workload", NK_TEXT_LEFT);
    nk_labelf(ctx, NK_TEXT_LEFT, "%u draw commands", r->ui.draw_count);
    nk_labelf(ctx, NK_TEXT_LEFT, "%u vertices | %u indices", r->ui.vertex_count, r->ui.index_count);
    nk_layout_row_dynamic(ctx, 24, 1);
    if (g_gpu_profiler_ui.show_pipeline_stats) {
        uint64_t total_vs = 0, total_fs = 0, total_primitives = 0;
        forEach(i, g_gpu_profiler_ui.pass_count) {
            total_vs += g_gpu_profiler_ui.pass_stats[i].vs_invocations;
            total_fs += g_gpu_profiler_ui.pass_stats[i].fs_invocations;
            total_primitives += g_gpu_profiler_ui.pass_stats[i].primitives;
        }
        char vs[32], fs[32], primitives[32];
        profiler_format_count(vs, sizeof(vs), total_vs);
        profiler_format_count(fs, sizeof(fs), total_fs);
        profiler_format_count(primitives, sizeof(primitives), total_primitives);
        nk_labelf(ctx, NK_TEXT_LEFT, "Vertices: %s | Fragments: %s | Clipping primitives: %s", vs, fs, primitives);
    }
    nk_labelf(ctx, NK_TEXT_LEFT, "Total GPU: %.3f ms | Average: %.3f ms", gpu_ms, g_gpu_profiler_ui.avg_total_gpu_time_ms);
    nk_layout_row_dynamic(ctx, 65, 1);
    if (nk_chart_begin(ctx, NK_CHART_LINES, GPU_PROF_HISTORY_SIZE, 0.0f,
                       MAX(1.0f, (float)g_gpu_profiler_ui.avg_total_gpu_time_ms * 1.5f))) {
        forEach(i, GPU_PROF_HISTORY_SIZE)
            nk_chart_push(ctx, g_gpu_profiler_ui.total_history[(g_gpu_profiler_ui.total_history_idx + i) % GPU_PROF_HISTORY_SIZE]);
        nk_chart_end(ctx);
    }
    nk_layout_row_dynamic(ctx, 22, 5);
    nk_label(ctx, "Pass", NK_TEXT_LEFT);
    nk_label(ctx, "Time (ms)", NK_TEXT_LEFT);
    nk_label(ctx, "Average", NK_TEXT_LEFT);
    nk_label(ctx, "Min / Max", NK_TEXT_LEFT);
    nk_label(ctx, "% Total", NK_TEXT_LEFT);
    forEach(i, g_gpu_profiler_ui.pass_count) {
        GpuPassStats *ps = &g_gpu_profiler_ui.pass_stats[i];
        nk_layout_row_dynamic(ctx, 22, 5);
        nk_label(ctx, ps->name, NK_TEXT_LEFT);
        if (ps->time_ms < 0.1)
            nk_labelf(ctx, NK_TEXT_LEFT, "%.1f us", ps->time_ms * 1000.0);
        else
            nk_labelf(ctx, NK_TEXT_LEFT, "%.3f ms", ps->time_ms);
        nk_labelf(ctx, NK_TEXT_LEFT, "%.3f", ps->avg_ms);
        nk_labelf(ctx, NK_TEXT_LEFT, "%.3f / %.3f", ps->min_ms, ps->max_ms);
        nk_labelf(ctx, NK_TEXT_LEFT, "%.1f%%", gpu_ms > 0.0 ? ps->time_ms / gpu_ms * 100.0 : 0.0);
        if (g_gpu_profiler_ui.show_pipeline_stats) {
            char vs[32], fs[32], primitives[32];
            profiler_format_count(vs, sizeof(vs), ps->vs_invocations);
            profiler_format_count(fs, sizeof(fs), ps->fs_invocations);
            profiler_format_count(primitives, sizeof(primitives), ps->primitives);
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_labelf(ctx, NK_TEXT_LEFT, "    VS %s | FS %s | Clipping primitives %s", vs, fs, primitives);
        }
    }
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_labelf(ctx, NK_TEXT_LEFT, "Timestamp Period: %.2f ns | Query Pool Size: %d passes",
              (double)r->vk.info.properties.limits.timestampPeriod, MAX_GPU_PASSES);
    nk_end(ctx);
}
