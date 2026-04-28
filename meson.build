project('haptika', 'c',
  version: '1.0.0',
  default_options: [
    'c_std=c11',
    'warning_level=2'
  ]
)

cc = meson.get_compiler('c')

adw_dep = dependency('libadwaita-1')
gtk_dep = dependency('gtk4')
sdl3_dep = dependency('sdl3', version: '>=3.2.0')
evdev_dep = dependency('libevdev', version: '>=1.11.0')
udev_dep = dependency('libudev', version: '>=249')
threads_dep = dependency('threads')
m_dep = cc.find_library('m', required : false)
pipewire_dep = dependency('libpipewire-0.3', required: false)

gnome = import('gnome')
i18n = import('i18n')

conf = configuration_data()
conf.set_quoted('HAPTIKA_VERSION', meson.project_version())
conf.set_quoted('HAPTIKA_APP_ID', 'io.github.gabriel2silva.Haptika')
conf.set_quoted('GETTEXT_PACKAGE', meson.project_name())
conf.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))
conf.set10('HAVE_PIPEWIRE', pipewire_dep.found())
config_h = configure_file(output: 'haptika-config.h', configuration: conf)

resources = gnome.compile_resources('haptika-resources',
  'data/haptika.gresource.xml',
  source_dir: 'data',
)



src_files = [
  'src/main.c',
  'src/window.c',
  'src/device_manager.c',
  'src/ds4_info.c',
  'src/ds5_info.c',
  'src/ds3_info.c',
  'src/controller_info_utils.c',
  'src/selector_view.c',
  'src/input_viewer.c',
  'src/input_session.c',
  'src/input_session_evdev.c',
  'src/input_session_sdl3.c',
  'src/battery.c',
  'src/speaker_test.c',
  'src/widgets/analog_stick.c',
  'src/widgets/button_indicator.c',
  'src/widgets/tilt_indicator.c',
  'src/widgets/touchpad.c',
  'src/widgets/trigger_bar.c'
]

haptika_deps = [adw_dep, gtk_dep, sdl3_dep, evdev_dep, udev_dep, threads_dep, m_dep]
if pipewire_dep.found()
  haptika_deps += pipewire_dep
endif

executable('haptika',
  src_files, resources, config_h,
  dependencies: haptika_deps,
  include_directories: include_directories('.'),
  install: true
)

install_data('data/io.github.gabriel2silva.Haptika.desktop',
  install_dir: get_option('datadir') / 'applications'
)

install_data('data/io.github.gabriel2silva.Haptika.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo'
)

install_data('data/icons/hicolor/scalable/apps/io.github.gabriel2silva.Haptika.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps'
)

install_data('data/icons/hicolor/symbolic/apps/io.github.gabriel2silva.Haptika-symbolic.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/symbolic/apps'
)

install_data('data/io.github.gabriel2silva.Haptika.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0/schemas'
)

gnome.post_install(
  gtk_update_icon_cache: true,
  glib_compile_schemas: true,
)

subdir('po')

# Validation tests (run with `meson test -C build`)
desktop_file_validate = find_program('desktop-file-validate', required: false)
if desktop_file_validate.found()
  test('validate-desktop', desktop_file_validate,
    args: [meson.project_source_root() / 'data/io.github.gabriel2silva.Haptika.desktop'])
endif

appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('validate-metainfo', appstreamcli,
    args: ['validate', '--no-net',
           meson.project_source_root() / 'data/io.github.gabriel2silva.Haptika.metainfo.xml'])
endif

# Developer tools (not installed, build with `meson compile -C build test_sdl` etc.)
executable('test_icon', 'tools/test_icon.c',
  dependencies: [adw_dep, gtk_dep],
  install: false,
)
executable('test_match', 'tools/test_match.c',
  dependencies: [udev_dep],
  install: false,
)
executable('test_sdl', 'tools/test_sdl.c',
  dependencies: [sdl3_dep],
  install: false,
)
executable('test_udev', 'tools/test_udev.c',
  dependencies: [udev_dep],
  install: false,
)
executable('test_udev_subsys', 'tools/test_udev_subsys.c',
  dependencies: [udev_dep],
  install: false,
)
