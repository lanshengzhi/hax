project('glaze', 'cpp',
    version: '8.1.0',
    default_options: ['cpp_std=c++23'],
)

# Glaze is header-only. Keep its include directory system-scoped so diagnostics and lint stay
# focused on hax; the project adapter includes only the JSON header family.
glaze_dep = declare_dependency(
    include_directories: include_directories('include', is_system: true),
)
