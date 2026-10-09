# Mock GenTL producers for the 'gentl' test suite, see arvgentlmock.h

gentl_mock_c_args = ['-DARAVIS_COMPILATION'] + gentl_glib_version_args

gentl_mock_variants = [
	['ArvGenTLMock',	[]],
	['ArvGenTLMockMinimal',	['-DARV_GENTL_MOCK_MINIMAL']],
	['ArvGenTLMockBroken',	['-DARV_GENTL_MOCK_BROKEN']],
]

gentl_mocks = {}
gentl_mock_targets = []

foreach variant: gentl_mock_variants
	gentl_mock = shared_module (variant[0], 'arvgentlmock.c',
				    name_prefix: '',
				    name_suffix: 'cti',
				    c_args: gentl_mock_c_args + variant[1],
				    link_with: aravis_library,
				    dependencies: aravis_public_dependencies,
				    include_directories: [library_inc])
	gentl_mocks += {variant[0]: gentl_mock}
	gentl_mock_targets += [gentl_mock]
endforeach

# A producer depending on a library installed next to it, for the Windows library search path
if host_machine.system() == 'windows'
	gentl_mock_dep_library = shared_library ('arvgentlmockhelper', 'arvgentlmockhelper.c')
	gentl_mock = shared_module ('ArvGenTLMockDep', 'arvgentlmock.c',
				    name_prefix: '',
				    name_suffix: 'cti',
				    c_args: gentl_mock_c_args + ['-DARV_GENTL_MOCK_DEP'],
				    link_with: [aravis_library, gentl_mock_dep_library],
				    dependencies: aravis_public_dependencies,
				    include_directories: [library_inc])
	gentl_mocks += {'ArvGenTLMockDep': gentl_mock}
	gentl_mock_targets += [gentl_mock, gentl_mock_dep_library]
endif
