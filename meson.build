project('hyprgrass', 'cpp', 'c',
  version: run_command('cat', join_paths(meson.project_source_root(), 'VERSION'), check: true).stdout().strip(),
  default_options: ['buildtype=release'],
)

cpp_compiler = meson.get_compiler('cpp')
if cpp_compiler.has_argument('-std=c++23')
  add_global_arguments('-std=c++23', language: 'cpp')
elif cpp_compiler.has_argument('-std=c++2b')
  add_global_arguments('-std=c++2b', language: 'cpp')
else
  error('Could not configure current C++ compiler (' + cpp_compiler.get_id() + ' ' + cpp_compiler.version() + ') with required C++ standard (C++23)')
endif

hyprland_headers = dependency('hyprland', required: false)
if not hyprland_headers.found()
  hyprland_src = run_command('printenv', 'HYPRLAND_HEADERS', check: false).stdout().strip()
  if hyprland_src == ''
    error('Hyprland not found via pkg-config and environment variable HYPRLAND_HEADERS not set')
  endif

  add_global_arguments('-I' + hyprland_src, language: 'cpp')
endif

if get_option('hyprgrass')
  wftouch = dependency('wftouch', required: false)

  if not wftouch.found() and get_option('hyprgrass')
    wftouch = subproject('wf-touch').get_variable('wftouch')
  endif
endif

# Hyprland 0.55+ embeds Lua 5.5 for its Lua config; plugins that expose
# functions to the `hl` table (via HyprlandAPI::addLuaFunction) need its headers.
lua_dep = dependency(
  'lua55', 'lua5.5', 'lua-5.5', 'lua',
  version: ['>=5.5', '<5.6'],
  required: false,
)

hyprland_deps = [
  dependency('pixman-1'),
  dependency('libinput'),
  dependency('wayland-server'),
  dependency('xkbcommon'),
  dependency('libdrm'),
]

if lua_dep.found()
  hyprland_deps += lua_dep
endif

subdir('examples')
subdir('src')
