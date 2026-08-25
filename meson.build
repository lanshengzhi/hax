project('hax', 'cpp',
    version: '0.4.0',
    license: 'MIT',
    default_options: [
        'cpp_std=c++23',
        'warning_level=3',
        'buildtype=debugoptimized',
    ],
)

# glibc hides POSIX 2008 interfaces until asked for them, while the BSD libcs
# expose POSIX by default and read these macros as a request to *hide* their
# own extensions — which takes flock(2)'s LOCK_* constants with them. Asking
# only where the request is needed keeps both families happy.
#
# _XOPEN_SOURCE=700 is SUSv4 (POSIX 2008 + XSI), needed on glibc to expose
# realpath() — _POSIX_C_SOURCE alone doesn't gate it through.
if host_machine.system() == 'linux'
    add_project_arguments(
        '-D_POSIX_C_SOURCE=200809L',
        '-D_XOPEN_SOURCE=700',
        language: 'cpp',
    )
endif

add_project_arguments(
    '-D_DARWIN_C_SOURCE',
    # Project headers go through -iquote, not -I, so `#include <foo.h>`
    # always reaches a system header rather than shadowing it with one
    # of ours (e.g. `src/util.h` would collide with macOS's `<util.h>`
    # under -I). The `tests/` entry serves the same purpose for
    # `harness.h` and friends.
    '-iquote', meson.project_source_root() / 'src',
    '-iquote', meson.project_source_root() / 'tests',
    language: 'cpp',
)

# include_type system turns the dependencies' -I flags into -isystem so
# clang-tidy does not report findings inside third-party headers (Homebrew
# installs them outside the default system include path).
libcurl = dependency('libcurl', include_type: 'system')
jansson = dependency('jansson', include_type: 'system')
glaze_project = subproject('glaze')
glaze_dep = glaze_project.get_variable('glaze_dep')
threads = dependency('threads')

# FreeBSD resolves isfinite() to a libm symbol rather than a compiler builtin,
# so the math library has to be linked explicitly there. Optional because
# platforms that fold the math functions into libc ship no separate library.
libm = meson.get_compiler('cpp').find_library('m', required: false)

fs = import('fs')

# The version baked into the binary. `--always` keeps pre-tag checkouts
# building (bare commit hash); the fallback covers builds without git, such as
# dist tarballs, where it must match the tag scheme's leading "v".
#
# Describe only against our own repository: otherwise git walks up and describes
# whatever encloses an extracted tarball. vcs_tag falls back when the command
# fails, not when it succeeds against the wrong repository, so a missing
# repository has to select the fallback by failing. Test existence rather than
# is_dir(): `.git` is a file in a worktree or submodule.
if fs.exists('.git')
    describe = ['git', 'describe', '--tags', '--always', '--dirty=+']
else
    describe = ['false']
endif

version_h = vcs_tag(
    command: describe,
    input: 'src/version.h.in',
    output: 'version.h',
    fallback: 'v' + meson.project_version(),
)

# Production sources, grouped by subdirectory and sorted within each group.
# `main.cpp` is deliberately excluded: it lives only in the `hax` executable so
# that test binaries (which bring their own `main`) can link the same objects
# via `hax_lib` without colliding. Keep these lists sorted — additions should
# slot into the right group alphabetically.
sources = files(
    'src/agent.cpp',
    'src/agent_core.cpp',
    'src/agent_dispatch.cpp',
    'src/agent_env.cpp',
    'src/agent_loop.cpp',
    'src/agent_tool.cpp',
    'src/agent_usage.cpp',
    'src/banner.cpp',
    'src/busy.cpp',
    'src/catalog.cpp',
    'src/catalog_json.cpp',
    'src/cli.cpp',
    'src/compact.cpp',
    'src/config.cpp',
    'src/cred_store.cpp',
    'src/effort.cpp',
    'src/file_mention.cpp',
    'src/history.cpp',
    'src/json.cpp',
    'src/login.cpp',
    'src/model_meta.cpp',
    'src/model_sort.cpp',
    'src/oneshot.cpp',
    'src/paste_image.cpp',
    'src/provider.cpp',
    'src/select.cpp',
    'src/session.cpp',
    'src/session_control.cpp',
    'src/session_item.cpp',
    'src/session_picker.cpp',
    'src/session_prune.cpp',
    'src/slash.cpp',
    'src/tool_schema.cpp',
    'src/trace.cpp',
    'src/transcript.cpp',
    'src/turn.cpp',
    'src/util.cpp',

    'src/providers/anthropic.cpp',
    'src/providers/anthropic_body.cpp',
    'src/providers/anthropic_events.cpp',
    'src/providers/anthropic_json.cpp',
    'src/providers/chat_body.cpp',
    'src/providers/chat_events.cpp',
    'src/providers/codex.cpp',
    'src/providers/codex_auth.cpp',
    'src/providers/codex_login.cpp',
    'src/providers/codex_settings.cpp',
    'src/providers/config_provider.cpp',
    'src/providers/http_provider.cpp',
    'src/providers/llamacpp.cpp',
    'src/providers/mock.cpp',
    'src/providers/openai.cpp',
    'src/providers/openai_common.cpp',
    'src/providers/openai_compat_json.cpp',
    'src/providers/opencode.cpp',
    'src/providers/openrouter.cpp',
    'src/providers/recipes.cpp',
    'src/providers/registry.cpp',
    'src/providers/responses_body.cpp',
    'src/providers/responses_events.cpp',
    'src/providers/stream_retry.cpp',
    'src/providers/usage_render.cpp',
    'src/providers/wire.cpp',

    'src/render/ctrl_strip.cpp',
    'src/render/diff_color.cpp',
    'src/render/disp.cpp',
    'src/render/markdown.cpp',
    'src/render/markdown_scan.cpp',
    'src/render/markdown_table.cpp',
    'src/render/markdown_wrap.cpp',
    'src/render/progress.cpp',
    'src/render/render_ctx.cpp',
    'src/render/spinner.cpp',
    'src/render/tool_render.cpp',

    'src/system/bg_job.cpp',
    'src/system/fs.cpp',
    'src/system/git.cpp',
    'src/system/keepawake.cpp',
    'src/system/os.cpp',
    'src/system/path.cpp',
    'src/system/spawn.cpp',
    'src/system/tempfiles.cpp',

    'src/terminal/clipboard.cpp',
    'src/terminal/input.cpp',
    'src/terminal/input_core.cpp',
    'src/terminal/interrupt.cpp',
    'src/terminal/notify.cpp',
    'src/terminal/picker.cpp',
    'src/terminal/picker_core.cpp',
    'src/terminal/theme.cpp',
    'src/terminal/ui.cpp',
    'src/terminal/vt_resolve.cpp',
    'src/terminal/width.cpp',

    'src/text/base64.cpp',
    'src/text/diff.cpp',
    'src/text/utf8.cpp',
    'src/text/utf8_sanitize.cpp',
    'src/text/width.cpp',

    'src/tools/bash.cpp',
    'src/tools/bash_cd_strip.cpp',
    'src/tools/bash_classify.cpp',
    'src/tools/bash_env.cpp',
    'src/tools/bash_output.cpp',
    'src/tools/bash_process.cpp',
    'src/tools/bash_shell.cpp',
    'src/tools/edit.cpp',
    'src/tools/image_sniff.cpp',
    'src/tools/output_cap.cpp',
    'src/tools/path_preprocess.cpp',
    'src/tools/read.cpp',
    'src/tools/task_registry.cpp',
    'src/tools/task_wait.cpp',
    'src/tools/write.cpp',

    'src/transport/api_error.cpp',
    'src/transport/ca.cpp',
    'src/transport/http.cpp',
    'src/transport/retry.cpp',
    'src/transport/sse.cpp',
)

# One archive shared by the executable and every test. Static-archive linking
# is lazy: each consumer pulls only the objects it transitively references, so
# tests get exactly the closure they need without hand-listing sources. The
# target is named distinctly from the `hax` executable so `meson compile -C
# build hax` stays unambiguous.
hax_lib = static_library('hax_lib',
    sources,
    version_h,
    dependencies: [libcurl, jansson, glaze_dep, threads, libm],
)

# Wrap the library as a dependency so consumers inherit both the link against
# `hax_lib` and the include dirs / compile flags of libcurl / jansson / Glaze / threads
# — their own translation units (`main.cpp`, each test) include <jansson.h> etc.
# and need those paths at compile time, which `link_with` alone wouldn't carry.
# Only the compile-side of each dependency is propagated here via
# partial_dependency(): the link args already ride along through `link_with`
# (static libraries propagate their link deps), so re-adding them would make
# every link emit `ld: warning: ignoring duplicate libraries`.
hax_dep = declare_dependency(
    link_with: hax_lib,
    dependencies: [
        libcurl.partial_dependency(compile_args: true, includes: true),
        jansson.partial_dependency(compile_args: true, includes: true),
        glaze_dep.partial_dependency(compile_args: true, includes: true),
        threads.partial_dependency(compile_args: true, includes: true),
    ],
)

hax_exe = executable('hax',
    'src/main.cpp',
    dependencies: [hax_dep],
    install: true,
)

subdir('tests')
