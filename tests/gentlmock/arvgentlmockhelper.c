/* SPDX-License-Identifier:Unlicense */

/*
 * Dependency of the ArvGenTLMockDep producer (Windows only). The 'gentl' tests copy both into a folder which is not in
 * the library search path: the producer only loads if its own folder is searched for its dependencies.
 */

__declspec(dllexport) int arv_gentl_mock_helper_get_answer (void);

int
arv_gentl_mock_helper_get_answer (void)
{
	return 42;
}
