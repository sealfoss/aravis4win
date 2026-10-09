
if get_option('tests')

  add_test_setup (
    'valgrind',
    exe_wrapper:[
      'valgrind',
      '--tool=memcheck',
      '--leak-check=yes',
      '--suppressions=' + meson.project_source_root() / 'tests' / 'aravis.supp',
      '--error-exitcode=1'
    ],
    # FAKEUV_TIME_SCALE: the fake libusb and the 'fakeuv' tests stretch their delays and timeouts
    env:['DEBUGINFOD_URLS=', 'ARV_TEST_IGNORE_BUFFER=', 'FAKEUV_TIME_SCALE=10'],
    # 'packaging' runs Python scripts on data files, no Aravis code;
    # 'benchmark' is a Python script that times processes;
    # 'gst' and 'fakeuvgst': GStreamer's own allocations are not suppressed. Instead, the gstaravis ownership cases
    # count the live ArvBuffers, and both suites were checked locally under MSVC's AddressSanitizer
    # (-Db_sanitize=address; no CI job builds with it)
    exclude_suites:['python', 'packaging', 'benchmark', 'gst', 'fakeuvgst'],
    timeout_multiplier:5
  )

	tests = [
		['evaluator',	['main'],    []],
		['buffer',	['main'],    []],
		['misc',	['main'],    []],
		['dom',		['main'],
                ['-DGENICAM_FILENAME="@0@/tests/data/genicam.xml"'.format (meson.project_source_root ())]],
		['genicam',	['main'],
                ['-DGENICAM_FILENAME="@0@/tests/data/genicam.xml"'.format (meson.project_source_root ())]],
		['fake',	['main'],
                ['-DGENICAM_FILENAME="@0@/src/arv-fake-camera.xml"'.format (meson.project_source_root ())]],
		['fakegv',	['network'],
                ['-DGENICAM_FILENAME="@0@/src/arv-fake-camera.xml"'.format (meson.project_source_root ())]]
	]

	foreach t: tests
		exe = executable (t[0], '@0@.c'.format (t[0]),
				  c_args: [t[2]],
				  link_with: aravis_library,
				  dependencies: aravis_dependencies,
				  include_directories: [library_inc])
		# A 'network' test runs a fake GigE camera, which binds the fixed GVCP port: never two at once, even when
		# repeated
		test (t[0], exe, suite: t[1], timeout: 60, is_parallel: not t[1].contains ('network'))
	endforeach

	# libusb-version checks the libusb loaded at run time against the Windows
	# version floor of meson.build (issue #431), and against
	# ARV_TEST_LIBUSB_MIN_VERSION and ARV_TEST_LIBUSB_HOTPLUG when CI jobs set them.
	if usb_dep.found()
		libusb_version_test = executable ('libusb-version', 'libusbversion.c',
						  dependencies: [glib_dep, usb_dep])
		test ('libusb-version', libusb_version_test, suite: 'usb', timeout: 30)
	endif
	# libusb-wrap checks that subprojects/libusb.wrap needs no patch or git
	# program, and that its build files only name files of the libusb sources.
	libusb_wrap_python = import ('python').find_installation ()
	test ('libusb-wrap', libusb_wrap_python,
	      args: [files ('libusbwrap.py'), meson.project_source_root ()],
	      suite: 'packaging', timeout: 30)

	# doc-pages checks that every page of docs/reference/aravis is published and
	# rebuilds the documentation when it changes, and that the links to sections
	# of the documentation, from its pages and from the sources, name sections
	# that exist.
	doc_pages_python = import ('python').find_installation ()
	test ('doc-pages', doc_pages_python,
	      args: [files ('docpages.py'), meson.project_source_root ()],
	      suite: 'packaging', timeout: 30)

	# WinUSB driver package for USB3 Vision cameras on Windows: checks
	# data/windows/aravis-u3v.inf, its MakeCat definition and its PowerShell
	# helper, with the Python that runs meson. On Windows, winusb-inf also runs
	# InfVerif and MakeCat when they are installed, and winusb-setup runs the
	# helper, with -WhatIf or on saved pnputil outputs: nothing is installed.
	winusb_python = import ('python').find_installation ()
	test ('winusb-inf', winusb_python,
	      args: [files ('winusbinf.py'), winusb_inf, winusb_cdf, winusb_setup_script],
	      suite: 'packaging', timeout: 60)
	if host_machine.system () == 'windows'
		test ('winusb-setup', winusb_python,
		      args: [files ('winusbsetup.py'), winusb_setup_script],
		      suite: 'packaging', timeout: 180)
	endif

	# USB3 Vision throughput benchmark: unit-tests the parser of
	# scripts/arv-usb-benchmark.py on outputs of arv-camera-test saved in
	# tests/data, its matrix, dry run and reports, and runs it against the Fake
	# camera with the arv-camera-test of the build: no camera, no network.
	benchmark_python = import ('python').find_installation ()
	test ('usb-benchmark', benchmark_python,
	      args: [files ('usbbenchmark.py'), meson.project_source_root (), aravis_programs['arv-camera-test']],
	      suite: 'benchmark', timeout: 120)

	# Debug output: the level symbols on Windows consoles, and the C runtime that writes the messages
	debug_test = executable ('debug', 'debug.c',
				 link_with: aravis_library,
				 dependencies: aravis_dependencies,
				 include_directories: [library_inc])
	test ('debug', debug_test, suite: ['main', 'debug'], timeout: 60)

	# GigE Vision packet size versus the MTU of the host interface (issue #1047).
	# gvpacketsize: unit tests of the packet size negotiation and of the interface MTU enumeration, no network traffic.
	# fakegvmtu: end to end with the fake GigE camera on loopback; it binds the fixed GVCP port, hence the 'network'
	# suite, like fakegv, and no concurrent test.
	gvpacketsize_exe = executable ('gvpacketsize', 'gvpacketsize.c',
				       c_args: ['-DARAVIS_COMPILATION'],
				       link_with: aravis_library,
				       dependencies: aravis_dependencies,
				       include_directories: [library_inc])
	test ('gvpacketsize', gvpacketsize_exe, suite: ['gige'], timeout: 60)

	fakegvmtu_exe = executable ('fakegvmtu', 'fakegvmtu.c',
				    c_args: ['-DGENICAM_FILENAME="@0@/src/arv-fake-camera.xml"'.format (meson.project_source_root ())],
				    link_with: aravis_library,
				    dependencies: aravis_dependencies,
				    include_directories: [library_inc])
	test ('fakegvmtu', fakegvmtu_exe, suite: ['network'], timeout: 120, is_parallel: false)

	# GigE Vision helpers, without network traffic.
	# gvsp: GVSP packet accessors, with G_LIKELY/G_UNLIKELY redefined so that "G_LIKELY (pointer) != NULL" fails to
	# build.
	# gvnetwork: subnet and host address checks used to reach a discovered device (the fakegv test asserts the
	# result), and extraction of UDP payloads from packet socket captures, which rejects IP fragments.
	foreach t: ['gvsp', 'gvnetwork']
		exe = executable (t, '@0@.c'.format (t),
				  c_args: ['-DARAVIS_COMPILATION'],
				  link_with: aravis_library,
				  dependencies: aravis_dependencies,
				  include_directories: [library_inc])
		test (t, exe, suite: ['gige'], timeout: 60)
	endforeach

	# GenTL consumer tests against the mock GenTL producers of tests/gentlmock. The producers link libaravis, which
	# needs a shared library: default_library 'shared' or 'both'.
	if build_shared
		# The GLib API of the minimal version Aravis requires: a newer function or enumerator is a warning
		gentl_glib_version_args = ['-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_58',
					   '-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_58']

		subdir ('gentlmock')

		gentl_test_env = environment ()
		gentl_test_env.set ('ARV_TEST_GENTL_MOCK', gentl_mocks['ArvGenTLMock'].full_path ())
		gentl_test_env.set ('ARV_TEST_GENTL_MOCK_MINIMAL', gentl_mocks['ArvGenTLMockMinimal'].full_path ())
		gentl_test_env.set ('ARV_TEST_GENTL_MOCK_BROKEN', gentl_mocks['ArvGenTLMockBroken'].full_path ())
		if host_machine.system() == 'windows'
			gentl_test_env.set ('ARV_TEST_GENTL_MOCK_DEP', gentl_mocks['ArvGenTLMockDep'].full_path ())
			gentl_test_env.set ('ARV_TEST_GENTL_MOCK_DEP_LIBRARY', gentl_mock_dep_library.full_path ())
		endif
		gentl_test_env.set ('ARV_TEST_ARV_TOOL', aravis_programs['arv-tool'].full_path ())
		gentl_test_env.set ('ARV_TEST_ARV_CAMERA_TEST', aravis_programs['arv-camera-test'].full_path ())
		gentl_test_env.set ('ARV_TEST_ARV_TEST', aravis_programs['arv-test'].full_path ())
		gentl_test_depends = gentl_mock_targets + [aravis_programs['arv-tool'], aravis_programs['arv-camera-test'],
							   aravis_programs['arv-test']]

		# Aravis's own GenTL producer (src/gentl), loaded by the consumer. Its test cases (/gentl/producer/*) are
		# skipped unless it is built: ARV_TEST_GENTL_PRODUCER_NOT_BUILT gives them the reason.
		if get_option('gentl-producer')
			gentl_test_env.set ('ARV_TEST_GENTL_PRODUCER', gentl_producer_library.full_path ())
			gentl_test_depends += [gentl_producer_library]
		else
			gentl_test_env.set ('ARV_TEST_GENTL_PRODUCER_NOT_BUILT', '1')
		endif

		# ARAVIS_COMPILATION, for the GenTL API declared by src/arvgentlprivate.h
		gentl_test = executable ('gentl', 'gentl.c',
					 c_args: ['-DARAVIS_COMPILATION'] + gentl_glib_version_args,
					 link_with: aravis_library,
					 dependencies: aravis_dependencies,
					 include_directories: [library_inc, include_directories ('gentlmock')])
		test ('gentl', gentl_test, suite: 'gentl', timeout: 120, env: gentl_test_env,
		      depends: gentl_test_depends)
	endif

	# USB3 Vision tests against a fake libusb (suite 'fakeuv'). The test programs link the object files of the
	# Aravis library (compiled once, for the library itself) and tests/fakeusb instead of the real libusb, whose
	# headers only are used. See tests/fakeusb/fakeusb.h for the fake's scripting API and tests/fakeuvtest.h for the
	# shared helpers.
	if usb_dep.found()
		fakeuv_usb_headers_dep = usb_dep.partial_dependency (compile_args: true, includes: true)

		fakeuv_fakeusb_library = static_library ('fakeusb',
			'fakeusb/fakeusb.c', 'fakeusb/fakeusbcamera.c', library_enums[1], library_private_enums[1],
			c_args: library_c_args,
			dependencies: [aravis_dependencies_without_usb, fakeuv_usb_headers_dep],
			include_directories: [library_inc])

		# link_whole, so that every object of the fake makes it into the test programs
		fakeuv_dep = declare_dependency (
			link_whole: [fakeuv_fakeusb_library],
			sources: [library_enums[1], library_private_enums[1]],
			dependencies: [aravis_dependencies_without_usb, fakeuv_usb_headers_dep],
			include_directories: [library_inc, include_directories ('fakeusb')])

		fakeuv_aravis_objects = aravis_library.extract_all_objects (recursive: true)
		fakeuv_inc = include_directories ('.')
		# aravissrc against the fake, built in gst/meson.build when the plugin is
		fakeuv_gst_sources = files ('fakeuvgst.c', 'fakeuvtest.c')

		fakeuv_c_args = library_c_args + ['-DGENICAM_FILENAME="@0@/src/arv-fake-camera.xml"'.format (meson.project_source_root ())]

		# One entry per test program: tests/<name>.c, built with tests/fakeuvtest.c
		fakeuv_tests = [
			'fakeuv',
			'fakeuvlibusb',
			'fakeuvdevice',
			'fakeuvdiscovery',
			'fakeuvstream',
			'fakeuvunplug',
			'fakeuvunplugsync',
			'fakeuvunplugrawio',
			'fakeuvrawio',
			'fakeuvpriority',
			'fakeuvsystem',
		]

		foreach t: fakeuv_tests
			exe = executable (t, '@0@.c'.format (t), 'fakeuvtest.c',
					  objects: fakeuv_aravis_objects,
					  c_args: fakeuv_c_args,
					  dependencies: fakeuv_dep)
			# A program takes 20 s at most on its own; loaded CI runners and sanitizer builds take several times
			# longer
			test (t, exe, suite: 'fakeuv', timeout: 180)
		endforeach

		# The functions of libusb 1.0.30 that Aravis calls, against the libusb of the build rather than the fake:
		# found in the libusb DLL on Windows, or called directly when libusb is linked statically
		uvlibusb_test = executable ('uvlibusb', 'uvlibusb.c',
					    objects: fakeuv_aravis_objects,
					    sources: [library_enums[1], library_private_enums[1]],
					    c_args: library_c_args,
					    dependencies: aravis_dependencies,
					    include_directories: [library_inc])
		test ('uvlibusb', uvlibusb_test, suite: 'usb', timeout: 30)

		# aravissrc and the device provider against the fake libusb (tests/fakeuvgst.c): the plugin is compiled
		# into the test program and registered statically, next to the Aravis objects and the fake
		if gst_enabled
			fakeuv_gst_check_dep = dependency ('gstreamer-check-1.0', required: false)
			if fakeuv_gst_check_dep.found ()
				fakeuv_gst = executable ('fakeuvgst', fakeuv_gst_sources, gst_source_files,
							 objects: fakeuv_aravis_objects,
							 c_args: fakeuv_c_args + gst_c_args + ['-DGST_PLUGIN_BUILD_STATIC'],
							 include_directories: [fakeuv_inc, gst_inc],
							 dependencies: [fakeuv_dep, fakeuv_gst_check_dep,
									dependency ('gstreamer-base-1.0'),
									dependency ('gstreamer-app-1.0')])
				test ('fakeuvgst', fakeuv_gst, suite: ['fakeuv', 'fakeuvgst'], timeout: 120,
				      env: ['GST_PLUGIN_SYSTEM_PATH_1_0=', 'GST_PLUGIN_PATH_1_0=', 'GST_PLUGIN_PATH=',
					    'GST_REGISTRY_1_0=' + meson.current_build_dir () / 'fakeuvgst-registry.bin'])
			endif
		endif
	endif

        py_script_config_data = configuration_data ()
        py_script_config_data.set ('GI_TYPELIB_PATH', meson.project_build_root() / 'src')
        py_script_config_data.set ('LD_LIBRARY_PATH', meson.project_build_root() / 'src')
        py_script_config_data.set ('GST_PLUGIN_PATH', meson.project_build_root() / 'gst')
        py_script_config_data.set ('FAKE_GENICAM_PATH', meson.project_source_root() / 'src' / 'arv-fake-camera.xml')
        py_script_config_data.set ('ARV_PRODUCER_PATH', get_option('gentl-producer') ? gentl_producer_library.full_path() : '')

        configure_file (input: 'pylaunch.in', output: 'pylaunch', configuration: py_script_config_data)
        configure_file (input: 'pylaunch-dbg.in', output: 'pylaunch-dbg', configuration: py_script_config_data)
        configure_file (input: 'jslaunch.in', output: 'jslaunch', configuration: py_script_config_data)
        configure_file (input: 'jslaunch-dbg.in', output: 'jslaunch-dbg', configuration: py_script_config_data)

	if introspection_enabled
		pymod = import ('python')

		py = pymod.find_installation ('python3', required: false)

		if py.found()
			python_tests = [
			  ['fake.py',		[]],
			  ['exception.py',	[]],
			  ['device.py',	        []],
			]

			environment = [
				'GI_TYPELIB_PATH=' + meson.project_build_root() / 'src',
				'LD_LIBRARY_PATH=' + meson.project_build_root() / 'src',
				'FAKE_GENICAM_PATH=' + meson.project_source_root() / 'src' / 'arv-fake-camera.xml'
			]

			foreach t: python_tests
				test (t[0], py, suite: 'python', args: files (t[0]), env: environment)
			endforeach
		endif

	endif

	examples = [
		['arv-network-test',		'arvnetworktest.c'],
		['arv-device-test',		'arvdevicetest.c'],
		['arv-genicam-test',		'arvgenicamtest.c'],
		['arv-evaluator-test',		'arvevaluatortest.c'],
		['arv-zip-test',		'arvziptest.c'],
		['arv-chunk-parser-test',	'arvchunkparsertest.c'],
		['arv-heartbeat-test',		'arvheartbeattest.c'],
		['arv-acquisition-test',	'arvacquisitiontest.c'],
		['arv-example',			'arvexample.c'],
		['arv-auto-packet-size-test',	'arvautopacketsizetest.c'],
		['arv-device-scan-test',	'arvdevicescantest.c'],
		['arv-roi-test',		'arvroitest.c'],
		['arv-multi-uv-test',		'arvmultiuvtest.c'],
		['time-test',			'timetest.c'],
		['load-http-test',		'loadhttptest.c'],
		['cpp-test',			'cpp.cc'],
	]

        if get_option ('v4l2').enabled()
          examples += [
		['v4l2-test',			'v4l2test.c'],
            ]
        endif

	if host_machine.system()=='linux'
		examples+=[['realtime-test','realtimetest.c']] # uses Linux RT API unavailable on other platforms
	endif

	foreach example: examples
		executable (example[0], example[1],
			link_with: aravis_library,
			dependencies: aravis_dependencies,
			include_directories: [library_inc])
	endforeach

	# GStreamer plugin tests: aravissrc and the device provider of the plugin built here, against the in-process fake
	# camera and the mock GenTL producer of tests/gentlmock. They need the optional gstreamer-check-1.0, and a shared
	# libaravis, which the test program and the plugin it loads share: a static build gives the plugin a copy of its own,
	# whose types clash with those of the test program. Like 'fakeuvgst', the 'gst' suite is left out of the valgrind
	# setup, which does not suppress GStreamer's own allocations: the ownership checks count the live ArvBuffers
	# instead.
	if gst_enabled and build_shared
		gst_check_dep = dependency ('gstreamer-check-1.0', required: false)

		if gst_check_dep.found ()
			gst_test_registry = meson.current_build_dir () / 'gst-registry.bin'

			# Load this build's plugin, with a registry of its own, besides GStreamer's own plugins
			gst_test_env = environment ()
			gst_test_env.set ('GST_PLUGIN_PATH_1_0', meson.project_build_root () / 'gst')
			gst_test_env.set ('GST_PLUGIN_PATH', meson.project_build_root () / 'gst')
			gst_test_env.set ('GST_REGISTRY_1_0', gst_test_registry)
			gst_test_env.set ('GST_REGISTRY', gst_test_registry)
			# Run the checks in a single process, as they must on Windows
			gst_test_env.set ('CK_FORK', 'no')
			# For the checks with a GenTL camera
			gst_test_env.set ('ARV_TEST_GENTL_MOCK', gentl_mocks['ArvGenTLMock'].full_path ())

			gst_test_c_args = [
				'-DGENICAM_FILENAME="@0@/src/arv-fake-camera.xml"'.format (meson.project_source_root ()),
				'-DGST_ARAVIS_PLUGIN_FILENAME="@0@"'.format ('/'.join (gst_plugin.full_path ().split ('\\')))
			]

			gst_test = executable ('gstaravis', 'gstaravis.c',
					       c_args: gst_test_c_args,
					       link_with: aravis_library,
					       dependencies: [gst_deps, gst_check_dep],
					       include_directories: [library_inc, include_directories ('gentlmock')])
			test ('gstaravis', gst_test, suite: 'gst', env: gst_test_env,
			      depends: [gst_plugin, gentl_mocks['ArvGenTLMock']], timeout: 120)

			# Its checks with the in-process fake GigEVision camera on 127.0.0.1, which binds the fixed
			# GigEVision ports, as the 'fakegv' test does: in the 'network' suite, and never in parallel. In the
			# 'gst' suite too, so that the valgrind setup leaves them out.
			gst_network_test_env = environment ()
			gst_network_test_env.set ('GST_PLUGIN_PATH_1_0', meson.project_build_root () / 'gst')
			gst_network_test_env.set ('GST_PLUGIN_PATH', meson.project_build_root () / 'gst')
			gst_network_test_env.set ('GST_REGISTRY_1_0', gst_test_registry)
			gst_network_test_env.set ('GST_REGISTRY', gst_test_registry)
			gst_network_test_env.set ('CK_FORK', 'no')
			gst_network_test_env.set ('GST_ARAVIS_TEST_NETWORK', '1')
			gst_network_test_env.set ('GST_CHECKS', 'test_network_*')
			test ('gstaravis-network', gst_test, suite: ['network', 'gst'], env: gst_network_test_env,
			      depends: gst_plugin, timeout: 120, is_parallel: false)
		endif
	endif

endif
