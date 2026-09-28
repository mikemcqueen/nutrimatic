# Conan's pkgconf searches only its own prefix, so system SDL2 is found
# through sdl2-config instead.  Without libsdl2-dev the GUI is skipped.
sdl2_dep = dependency('sdl2', method: 'config-tool', required: false,
  include_type: 'system')

if sdl2_dep.found()
  imgui_dep = dependency('imgui', include_type: 'system')

  # generate() in conanfile.py copies these out of the imgui package.
  backends_dir = meson.global_build_root() / 'dep-info' / 'imgui-backends'
  imgui_backends_lib = static_library('imgui-backends',
    backends_dir / 'imgui_impl_sdl2.cpp',
    backends_dir / 'imgui_impl_sdlrenderer2.cpp',
    dependencies: [imgui_dep, sdl2_dep],
    override_options: ['warning_level=0', 'werror=false'])
  imgui_backends_dep = declare_dependency(
    link_with: imgui_backends_lib,
    include_directories: include_directories(backends_dir, is_system: true),
    dependencies: [imgui_dep, sdl2_dep])

  executable('pgui', 'pgui.cpp', 'column.cpp', 'list-box.cpp',
    dependencies: imgui_backends_dep, install: true)
endif
