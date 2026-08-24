# Each entry is just the test's source path; everything else is derived. The
# registered test name is the path with the `test_` prefix and `.c` suffix
# stripped, keeping the subdirectory (so `tools/test_read.cpp` -> `tools/read`,
# `transport/test_sse.cpp` -> `transport/sse`, top-level `test_util.cpp` -> `util`).
# That makes this list a direct mirror of the `sources` list in the top-level
# meson.build — keep it grouped by subdirectory and sorted within each group.
#
# Every test is a single .cpp file linked against `hax_dep`: lazy static-archive
# linking pulls in only the production objects it transitively references, and
# the dependency carries the libcurl/jansson/threads include dirs and link
# flags — so a test needs nothing here but its source path.
test_sources = [
    'test_agent.cpp',
    'test_agent_core.cpp',
    'test_agent_dispatch.cpp',
    'test_agent_env.cpp',
    'test_agent_loop.cpp',
    'test_agent_tool.cpp',
    'test_agent_usage.cpp',
    'test_banner.cpp',
    'test_catalog.cpp',
    'test_catalog_fetch.cpp',
    'test_cli.cpp',
    'test_compact.cpp',
    'test_config.cpp',
    'test_cred_store.cpp',
    'test_effort.cpp',
    'test_file_mention.cpp',
    'test_history.cpp',
    'test_json.cpp',
    'test_model_meta.cpp',
    'test_model_sort.cpp',
    'test_harness.cpp',
    'test_oneshot.cpp',
    'test_paste_image.cpp',
    'test_provider.cpp',
    'test_select_config.cpp',
    'test_select_model.cpp',
    'test_session.cpp',
    'test_session_prune.cpp',
    'test_slash.cpp',
    'test_tool_schema.cpp',
    'test_trace.cpp',
    'test_transcript.cpp',
    'test_turn.cpp',
    'test_util.cpp',

    'providers/test_anthropic_body.cpp',
    'providers/test_anthropic_events.cpp',
    'providers/test_anthropic_models.cpp',
    'providers/test_chat_body.cpp',
    'providers/test_chat_events.cpp',
    'providers/test_codex.cpp',
    'providers/test_codex_auth.cpp',
    'providers/test_codex_login.cpp',
    'providers/test_codex_settings.cpp',
    'providers/test_config_provider.cpp',
    'providers/test_http_provider.cpp',
    'providers/test_llamacpp.cpp',
    'providers/test_mock.cpp',
    'providers/test_model_info.cpp',
    'providers/test_openai.cpp',
    'providers/test_opencode.cpp',
    'providers/test_registry.cpp',
    'providers/test_responses_body.cpp',
    'providers/test_responses_events.cpp',
    'providers/test_stream_retry.cpp',
    'providers/test_wire.cpp',

    'render/test_ctrl_strip.cpp',
    'render/test_diff_color.cpp',
    'render/test_disp.cpp',
    'render/test_markdown.cpp',
    'render/test_markdown_scan.cpp',
    'render/test_markdown_table.cpp',
    'render/test_markdown_wrap.cpp',
    'render/test_render_ctx.cpp',
    'render/test_spinner.cpp',
    'render/test_tool_render.cpp',

    'system/test_bg_job.cpp',
    'system/test_fs.cpp',
    'system/test_git.cpp',
    'system/test_keepawake.cpp',
    'system/test_os.cpp',
    'system/test_path.cpp',
    'system/test_spawn.cpp',
    'system/test_tempfiles.cpp',

    'terminal/test_clipboard.cpp',
    'terminal/test_input.cpp',
    'terminal/test_input_core.cpp',
    'terminal/test_interrupt.cpp',
    'terminal/test_picker_core.cpp',
    'terminal/test_theme.cpp',
    'terminal/test_vt_resolve.cpp',
    'terminal/test_width.cpp',

    'text/test_base64.cpp',
    'text/test_diff.cpp',
    'text/test_utf8.cpp',
    'text/test_utf8_sanitize.cpp',
    'text/test_width.cpp',

    'tools/test_bash.cpp',
    'tools/test_bash_cd_strip.cpp',
    'tools/test_bash_classify.cpp',
    'tools/test_edit.cpp',
    'tools/test_image_sniff.cpp',
    'tools/test_output_cap.cpp',
    'tools/test_path_preprocess.cpp',
    'tools/test_read.cpp',
    'tools/test_task.cpp',
    'tools/test_task_wait.cpp',
    'tools/test_write.cpp',

    'transport/test_api_error.cpp',
    'transport/test_ca.cpp',
    'transport/test_http.cpp',
    'transport/test_retry.cpp',
    'transport/test_sse.cpp',
]

fs = import('fs')
foreach src : test_sources
    stem = fs.stem(src).substring(5)  # drop the leading 'test_'
    parent = fs.parent(src)
    name = parent == '.' ? stem : parent / stem
    exe = executable('test_' + name.underscorify(),
        src,
        dependencies: [hax_dep],
    )
    test(name, exe)
endforeach

# End-to-end scenarios: Python scripts under e2e/ that drive the built binary
# through the mock provider (see e2e/harness.py). Naming mirrors the C tests:
# e2e/test_oneshot_text.py registers as e2e/oneshot_text, so the name prefix
# selects the group (`meson test 'e2e/*'`).
e2e_scenarios = [
    'oneshot_text',
    'oneshot_tool',
]

python = find_program('python3')
foreach name : e2e_scenarios
    test('e2e' / name,
        python,
        args: [files('e2e/test_' + name + '.py')],
        env: {'HAX_BIN': hax_exe.full_path()},
        depends: hax_exe,
        timeout: 60,
    )
endforeach
